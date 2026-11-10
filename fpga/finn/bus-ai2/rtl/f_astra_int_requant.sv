// F-ASTRA integer requantization with run-time parameters: the post-MAC (and residual) requantization of an integer
// contract v2/v3 layer (integer_contract._requant_mixed, the FINN lowering's Add / BitShift / Max / Mul chain of
// finn_frontend_mixed) per channel c:
//   out = clip((acc + offset_c) >> shift_c, LOW, HIGH)      arithmetic shift (floor), offset_c = bias_c + 2^(shift_c - 1)
//   LOW = 0 with ReLU, else -2^(OW-1); HIGH = 2^(OW-1) - 1     (ties-up rounding, ReLU and saturation of the contract)
// PE lanes per beat (channels k PE .. k PE + PE - 1 of word k, lane l at [l IW +: IW] / [l OW +: OW]), one beat a
// clock; a frame is ROWS vectors of C channels (ROWS C / PE beats).
// Every width follows from the synthesis fields: accumulators of AW bits (the input elements, IW bits, IS signed, fit
// them), offsets of OFW signed bits, shifts SH_MIN .. SH_MIN + 2^SDW - 1 and OW-bit outputs. The arithmetic is exact
// for any parameter inside the fields: the sum has max(AW, OFW) + 1 bits and the shift and clip work on all of them,
// so the fields, not the parameter values, size the datapath.
// Free-running four-stage pipeline (parameter read, sum, shift, clip) into a shift-register output queue of Q beats.
// The input is taken while fewer than Q beats are inside (pipeline and queue), so s_ready is a register function and
// no ready path runs through the core; at a full-rate sink the queue holds the pipeline's beats and the core takes
// one beat every clock.
// Run-time parameters: an AXI4-Lite slave (32-bit data, AXIL_AW address bits) writes every channel's offset and shift
// (one distributed RAM per lane, D = C / PE words {shift - SH_MIN, offset}, power-on contents {PARAM_PATH}{l}.mem) and
// the layer's ReLU flag (power-on RELU). ap_rst_n does not change them (it clears HOLD, the sticky errors and WRITES
// only), so written parameters survive an accelerator soft reset. A value outside its synthesis field (offset outside
// OFW signed bits, shift outside the window, a ReLU flag other than 0 or 1) is refused with ERR_RANGE and nothing
// changes; it is never narrowed.
// Writes are taken only while held: CTRL.HOLD keeps the next frame back (no beat is taken at a frame boundary), and
// STATUS.HELD rises once no frame is in progress (at a frame boundary, pipeline and queue empty); any other parameter
// write is dropped and sets the sticky STATUS.ERR_BUSY. Channel parameters read back as committed through the compute
// read port, so only while STATUS.HELD (else 0); the ReLU flag reads back at any time. Register map (byte offsets;
// f_astra.finn_requant_params holds the same constants):
//   0x0000 ID 0x52510001 | 0x0004 CTRL bit0 HOLD | 0x0008 STATUS bit0 HELD, bit1 IDLE, bit8 ERR_BUSY, bit9 ERR_RANGE,
//   bit10 ERR_ACCESS (write 1 to clear) | 0x000C WRITES accepted parameter writes (a write clears it) |
//   0x0010 {OW, PE, C[15:0]} | 0x0014 {SH_MIN, SDW, OFW, AW} | 0x0018 {0, AXIL_AW, IS, IW} (8 bits each) |
//   0x001C ROWS | 0x0020 LAYER bit0 RELU (a parameter: written while held)
//   0x1000 + 16 (k 2^clog2(PE) + l): channel c = k PE + l: +0 offset bits 31:0 (staged), +4 offset bits 63:32 (staged),
//   +8 shift (the whole shift, not the field; commits the channel with the staged offset)
// Every response is OKAY; partial strobes, read-only or unmapped addresses set ERR_ACCESS.
`timescale 1ns/1ps
module f_astra_int_requant #(
    parameter integer C = 32,           // channels
    parameter integer PE = 1,           // channels per beat; divides C
    parameter integer ROWS = 1,         // vectors per frame
    parameter integer IW = 22,          // input element width in the stream
    parameter integer IS = 1,           // input elements signed (1) or unsigned (0)
    parameter integer AW = 22,          // accumulator field: IW + (IS ? 0 : 1) <= AW
    parameter integer OW = 8,           // output code width (signed)
    parameter integer OFW = 23,         // offset field: signed OFW bits, 2 .. 64
    parameter integer SDW = 4,          // shift field bits, 1 .. 6
    parameter integer SH_MIN = 0,       // smallest shift
    parameter integer RELU = 0,         // power-on ReLU flag
    parameter PARAM_PATH = "requant_",  // lane l: {PARAM_PATH}{l}.mem, C / PE words {shift - SH_MIN, offset}
    parameter integer Q = 16,           // output queue beats, at least 8 (pipeline + queue for one beat a clock)
    parameter integer AXIL_AW = 13      // byte address bits, at least clog2(0x1000 + 16 (C / PE) 2^clog2(PE))
)(
    input  wire clk,
    input  wire rst_n,
    input  wire s_valid,
    output wire s_ready,
    input  wire [PE*IW-1:0] s_data,
    output wire m_valid,
    input  wire m_ready,
    output wire [PE*OW-1:0] m_data,
    input  wire [AXIL_AW-1:0] s_axil_awaddr,
    input  wire s_axil_awvalid,
    output wire s_axil_awready,
    input  wire [31:0] s_axil_wdata,
    input  wire [3:0] s_axil_wstrb,
    input  wire s_axil_wvalid,
    output wire s_axil_wready,
    output wire [1:0] s_axil_bresp,
    output wire s_axil_bvalid,
    input  wire s_axil_bready,
    input  wire [AXIL_AW-1:0] s_axil_araddr,
    input  wire s_axil_arvalid,
    output wire s_axil_arready,
    output wire [31:0] s_axil_rdata,
    output wire [1:0] s_axil_rresp,
    output wire s_axil_rvalid,
    input  wire s_axil_rready
);
    localparam integer D = C / PE;                                  // words per lane RAM
    localparam integer KW = (D > 1) ? $clog2(D) : 1;                // word index
    localparam integer LW = (PE > 1) ? $clog2(PE) : 0;              // lane bits of the register map
    localparam integer LWS = (LW > 0) ? LW : 1;
    localparam integer FW = (ROWS > 1) ? $clog2(ROWS) : 1;          // vector index
    localparam integer RW = SDW + OFW;                              // RAM word {shift field, offset}
    localparam integer SW = ((AW > OFW) ? AW : OFW) + 1;            // acc + offset, exact
    localparam integer UW = (SW > SH_MIN) ? SW - SH_MIN : 1;        // after the fixed part of the shift
    localparam integer CNTW = $clog2(Q + 1);
    localparam integer QAW = (Q > 1) ? $clog2(Q) : 1;
    localparam signed [63:0] HIGH = (64'sd1 <<< (OW - 1)) - 1;
    localparam signed [63:0] NEG = -(64'sd1 <<< (OW - 1));

    // ------------------------------------------------------------------ input admission and frame position
    reg [KW-1:0] kc;                    // word (channel group) of the next beat
    reg [FW-1:0] vc;                    // vector of the next beat
    reg [CNTW-1:0] held_beats;          // beats taken and not yet given out (pipeline and queue)
    wire hold;                          // CTRL.HOLD: keep the next frame back
    wire boundary = (kc == {KW{1'b0}}) && (vc == {FW{1'b0}});
    wire admit = !(hold && boundary);
    assign s_ready = rst_n && (held_beats < Q) && admit;
    wire accept = s_valid && s_ready;
    wire give = m_valid && m_ready;
    wire frame_idle = boundary && (held_beats == {CNTW{1'b0}});

    always @(posedge clk) begin
        if (!rst_n) begin
            kc <= {KW{1'b0}}; vc <= {FW{1'b0}}; held_beats <= {CNTW{1'b0}};
        end else begin
            if (accept) begin
                if (kc == D - 1) begin
                    kc <= {KW{1'b0}};
                    vc <= (vc == ROWS - 1) ? {FW{1'b0}} : vc + 1'b1;
                end else kc <= kc + 1'b1;
            end
            held_beats <= held_beats + (accept ? 1'b1 : 1'b0) - (give ? 1'b1 : 1'b0);
        end
    end

    // ------------------------------------------------------------------ parameter RAMs (one per lane)
    wire prm_we;                        // AXI: commit of a channel
    wire [KW-1:0] prm_wk;
    wire [LWS-1:0] prm_wl;
    wire [RW-1:0] prm_wd;
    wire prm_read;                      // held: the read port serves AXI reads of word prm_rk
    wire [KW-1:0] prm_rk;
    wire [RW*PE-1:0] q0;                // parameters of the beat in stage 0 (or of the AXI read word)
    genvar l;
    generate for (l = 0; l < PE; l = l + 1) begin : g_lane_ram
        (* ram_style = "distributed" *) reg [RW-1:0] mem [0:D-1];
        initial $readmemh($sformatf("%s%0d.mem", PARAM_PATH, l), mem);
        reg [RW-1:0] q;
        always @(posedge clk) if (prm_we && (LW == 0 || prm_wl == l)) mem[prm_wk] <= prm_wd;
        always @(posedge clk) q <= mem[prm_read ? prm_rk : kc];
        assign q0[l*RW +: RW] = q;
    end endgenerate

    // ------------------------------------------------------------------ datapath
    reg relu_r = (RELU != 0);           // power-on ReLU flag; a written flag survives ap_rst_n. It changes only while
                                        // held (no beat inside), so the clip stage reads it directly
    reg v0, v1, v2, v3;
    reg [PE*IW-1:0] x0;
    reg [PE*SW-1:0] sum1;
    reg [PE*SDW-1:0] sh1;
    reg [PE*UW-1:0] u2;
    reg [PE*OW-1:0] y3;

    always @(posedge clk) begin
        if (!rst_n) begin
            v0 <= 1'b0; v1 <= 1'b0; v2 <= 1'b0; v3 <= 1'b0;
        end else begin
            v0 <= accept; v1 <= v0; v2 <= v1; v3 <= v2;
        end
        if (accept) x0 <= s_data;
    end

    generate for (l = 0; l < PE; l = l + 1) begin : g_lane
        wire [IW-1:0] xi = x0[l*IW +: IW];
        wire signed [SW-1:0] acc = (IS != 0) ? $signed({{(SW-IW){xi[IW-1]}}, xi}) : $signed({{(SW-IW){1'b0}}, xi});
        wire [RW-1:0] p = q0[l*RW +: RW];
        wire signed [SW-1:0] off = $signed({{(SW-OFW){p[OFW-1]}}, p[OFW-1:0]});
        wire signed [SW-1:0] s1 = sum1[l*SW +: SW];
        wire signed [UW-1:0] fixed = (SW > SH_MIN) ? (s1 >>> SH_MIN) : (s1 >>> (SW - 1));
        wire signed [UW-1:0] u = u2[l*UW +: UW];
        wire [OW-1:0] y;
        if (UW > OW) begin : g_clip
            wire [UW-OW:0] top = u[UW-1:OW-1];          // the bits that must equal the sign
            wire positive_over = !u[UW-1] && (|top);
            wire negative_over = u[UW-1] && !(&top);
            assign y = positive_over ? HIGH[OW-1:0] : (u[UW-1] && relu_r) ? {OW{1'b0}} :
                       negative_over ? NEG[OW-1:0] : u[OW-1:0];
        end else begin : g_wide
            wire signed [OW-1:0] e = u;                 // sign extension: every value fits
            assign y = (u[UW-1] && relu_r) ? {OW{1'b0}} : e;
        end
        always @(posedge clk) begin
            sum1[l*SW +: SW] <= acc + off;
            sh1[l*SDW +: SDW] <= p[OFW +: SDW];
            u2[l*UW +: UW] <= fixed >>> sh1[l*SDW +: SDW];
            y3[l*OW +: OW] <= y;
        end
    end endgenerate

    // ------------------------------------------------------------------ output queue (shift register)
    reg [PE*OW-1:0] srl [0:Q-1];
    reg [CNTW-1:0] stored;
    integer i;
    always @(posedge clk) if (v3) begin
        srl[0] <= y3;
        for (i = 1; i < Q; i = i + 1) srl[i] <= srl[i-1];
    end
    always @(posedge clk) begin
        if (!rst_n) stored <= {CNTW{1'b0}};
        else stored <= stored + (v3 ? 1'b1 : 1'b0) - (give ? 1'b1 : 1'b0);
    end
    wire [QAW-1:0] head = stored[QAW-1:0] - 1'b1;
    assign m_valid = (stored != {CNTW{1'b0}});
    assign m_data = srl[head];

    // ------------------------------------------------------------------ run-time parameters (AXI4-Lite)
    localparam [AXIL_AW-1:0] CHANNELS = 'h1000;
    localparam integer SH_MAX = SH_MIN + (1 << SDW) - 1;
    localparam [31:0] ID = 32'h52510001;
    reg hold_r, err_busy, err_range, err_access;
    reg [31:0] writes;
    reg aw_full, w_full, ar_full, rd_wait, bvalid, rvalid;
    reg [AXIL_AW-1:0] aw_addr, ar_addr;
    reg [31:0] w_data, rdata, off_lo, off_hi;
    reg [3:0] w_strb;
    reg rd_ok;                          // the parameter read was taken while held

    wire held = hold_r && frame_idle;
    wire [31:0] status = {21'd0, err_access, err_range, err_busy, 6'd0, frame_idle, held};
    // write decode (registered address and data)
    wire w_full_strb = (w_strb == 4'hF);
    wire w_ctrl = aw_addr < CHANNELS;
    wire [AXIL_AW-1:0] w_rel = aw_addr - CHANNELS;
    wire [AXIL_AW-1:0] w_slot = w_rel >> 4;
    wire [KW-1:0] w_k = w_slot >> LW;
    wire [LWS-1:0] w_l = (LW > 0) ? w_slot[LWS-1:0] : 1'b0;
    wire w_chan = !w_ctrl && ((w_slot >> LW) < D) && (w_l < PE) && (w_rel[3:2] != 2'd3);
    wire w_layer = w_ctrl && (aw_addr[11:2] == 10'd8);
    wire w_param = w_chan || w_layer;
    wire w_commit = w_chan && (w_rel[3:2] == 2'd2);
    wire [63:0] off_new = {off_hi, off_lo};
    wire [31:0] sh_rel = w_data - SH_MIN;
    wire chan_ok = ((&off_new[63:OFW-1]) || !(|off_new[63:OFW-1])) && w_data >= SH_MIN && w_data <= SH_MAX;
    wire value_ok = w_commit ? chan_ok : w_layer ? (w_data <= 32'd1) : 1'b1;
    wire do_write = aw_full && w_full && !bvalid;
    wire take = do_write && w_full_strb && w_param && held && value_ok;
    // read decode
    wire r_ctrl = ar_addr < CHANNELS;
    wire [AXIL_AW-1:0] r_rel = ar_addr - CHANNELS;
    wire [AXIL_AW-1:0] r_slot = r_rel >> 4;
    wire [LWS-1:0] r_l = (LW > 0) ? r_slot[LWS-1:0] : 1'b0;
    wire r_chan = !r_ctrl && ((r_slot >> LW) < D) && (r_l < PE) && (r_rel[3:2] != 2'd3);
    wire [RW-1:0] r_word = q0[r_l * RW +: RW];
    wire [OFW-1:0] r_off = r_word[OFW-1:0];
    wire [63:0] r_off64 = {{(64 - OFW){r_off[OFW-1]}}, r_off};
    reg [31:0] read_value;
    always @* begin
        read_value = 32'd0;
        if (r_ctrl) begin
            case (ar_addr[11:2])
                10'd0: read_value = ID;
                10'd1: read_value = {31'd0, hold_r};
                10'd2: read_value = status;
                10'd3: read_value = writes;
                10'd4: read_value = {OW[7:0], PE[7:0], C[15:0]};
                10'd5: read_value = {SH_MIN[7:0], SDW[7:0], OFW[7:0], AW[7:0]};
                10'd6: read_value = {8'd0, AXIL_AW[7:0], IS[7:0], IW[7:0]};
                10'd7: read_value = ROWS;
                10'd8: read_value = {31'd0, relu_r};
                default: read_value = 32'd0;
            endcase
        end else if (rd_ok && r_chan) begin
            case (r_rel[3:2])
                2'd0: read_value = r_off64[31:0];
                2'd1: read_value = r_off64[63:32];
                default: read_value = SH_MIN + r_word[OFW +: SDW];
            endcase
        end
    end

    always @(posedge clk) begin
        if (!rst_n) begin
            hold_r <= 1'b0; err_busy <= 1'b0; err_range <= 1'b0; err_access <= 1'b0; writes <= 32'd0;
            aw_full <= 1'b0; w_full <= 1'b0; ar_full <= 1'b0; rd_wait <= 1'b0; bvalid <= 1'b0; rvalid <= 1'b0;
        end else begin
            if (s_axil_awvalid && !aw_full) begin aw_full <= 1'b1; aw_addr <= s_axil_awaddr; end
            if (s_axil_wvalid && !w_full) begin w_full <= 1'b1; w_data <= s_axil_wdata; w_strb <= s_axil_wstrb; end
            if (s_axil_arvalid && !ar_full) begin ar_full <= 1'b1; ar_addr <= s_axil_araddr; end
            if (bvalid && s_axil_bready) bvalid <= 1'b0;
            if (rvalid && s_axil_rready) rvalid <= 1'b0;
            if (do_write) begin
                aw_full <= 1'b0; w_full <= 1'b0; bvalid <= 1'b1;
                if (!w_full_strb) err_access <= 1'b1;
                else if (w_ctrl && !w_layer) begin
                    case (aw_addr[11:2])
                        10'd1: hold_r <= w_data[0];
                        10'd2: begin
                            if (w_data[8]) err_busy <= 1'b0;
                            if (w_data[9]) err_range <= 1'b0;
                            if (w_data[10]) err_access <= 1'b0;
                        end
                        10'd3: writes <= 32'd0;
                        default: err_access <= 1'b1;
                    endcase
                end
                else if (!w_param) err_access <= 1'b1;
                else if (!held) err_busy <= 1'b1;                    // a frame is in progress or HOLD is off
                else if (!value_ok) err_range <= 1'b1;               // outside its synthesis field: refused
                else begin
                    writes <= writes + 1'b1;
                    if (w_chan && w_rel[3:2] == 2'd0) off_lo <= w_data;
                    if (w_chan && w_rel[3:2] == 2'd1) off_hi <= w_data;
                    if (w_layer) relu_r <= w_data[0];
                end
            end
            // a read takes the port when no write does; the data is there one cycle later
            if (ar_full && !rvalid && !rd_wait && !do_write) begin rd_wait <= 1'b1; rd_ok <= held; end
            if (rd_wait) begin
                rd_wait <= 1'b0; ar_full <= 1'b0; rvalid <= 1'b1; rdata <= read_value;
            end
        end
    end
    assign prm_we = take && w_commit;
    assign prm_wk = w_k;
    assign prm_wl = w_l;
    assign prm_wd = {sh_rel[SDW-1:0], off_new[OFW-1:0]};
    assign prm_read = held;
    assign prm_rk = r_slot >> LW;
    assign hold = hold_r;
    assign s_axil_awready = rst_n && !aw_full;
    assign s_axil_wready = rst_n && !w_full;
    assign s_axil_bresp = 2'b00;
    assign s_axil_bvalid = bvalid;
    assign s_axil_arready = rst_n && !ar_full;
    assign s_axil_rdata = rdata;
    assign s_axil_rresp = 2'b00;
    assign s_axil_rvalid = rvalid;
endmodule
