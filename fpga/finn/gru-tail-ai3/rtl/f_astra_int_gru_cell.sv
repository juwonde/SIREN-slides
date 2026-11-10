// F-ASTRA integer GRU cell: the SIREN Model 3 integer predictor contract (f_astra.gru_predictor.Integer.step) from
// the gate accumulators to the new hidden codes, one hidden unit per clock (f_astra.finn_gru_step.cell is the same
// arithmetic in Python).
// Streams, one beat per hidden unit j of a frame of H units:
//   x (in0)  x-part accumulators of contract rows j, H+j, 2H+j (r, z, n), element g at [g*XW +: XW]
//   a (in1)  hidden-part accumulators of contract rows 3H+j, 4H+j, 5H+j, element g at [g*HAW +: HAW]
//   h (in2)  the unit's current hidden code; taken into a queue of HQ codes as it comes (a fork in front of the cell
//            never waits on the cell), then used in step with the accumulators
//   m (out)  the unit's new hidden code
// Per unit (rne: round half to even):
//   A_i = rne((acc_i * mul_i + add_i) / 2^shift_i)              six rows, parameters in PARAM_FILE (one word per unit)
//   g_r = #(SIG <= A_rx + A_rh), g_z = #(SIG <= A_zx + A_zh)     2^BG - 1 sorted sigmoid thresholds
//   pre = A_nx + rne(g_r * A_nh / 2^BG)
//   n   = #(TANH <= pre) - 2^(BH-1)                               2^BH - 1 sorted tanh thresholds
//   h'  = clip(n + rne(g_z * (h - n) / 2^BG), BH bits)
// A threshold count is a pipelined binary search: the step for count bit b reads T[c + 2^b] from its own slice of the
// table (c holds the bits above b), so every slice is read once per unit. Slice files: {PATH}{b}.mem.
// Stall-all pipeline of LAT = 10 + 2 BG + 2 BH stages: every stage advances when the output is free or empty.
// No DSP blocks: every product is fabric logic (use_dsp "no"), so the cell fits a bitstream without spare DSPs.
// Every width follows from the synthesis fields (XW, HAW, MULW, OFW, SDW, SH_MIN, TW): the arithmetic is exact for
// any parameter inside them, so the fields, not the parameter values, size the datapath.
// Run-time parameters (AXIL = 1): an AXI4-Lite slave (32-bit data, AXIL_AW address bits) writes the requantization
// rows (multiplier, offset, shift of every gate row: the parameter memory becomes six RAMs, one per row group, power-on
// contents {PARAM_PATH}{r}.mem) and every sigmoid and tanh threshold (each search slice becomes a RAM with a write
// port; the two sigmoid searchers' copies are written together; block or distributed RAM as the ROM was). Power-on
// contents are the synthesis files; ap_rst_n does not change them (it clears HOLD, the sticky errors and WRITES only).
// A value outside its synthesis field (multiplier wider than MULW, offset outside OFW signed bits, shift outside
// SH_MIN .. SH_MIN + 2^SDW - 1, threshold outside TW signed bits) is refused with ERR_RANGE and nothing changes; it is
// never narrowed. The search inputs are saturated to TW + 1 signed bits in this mode: a value at or above the largest
// TW-bit threshold, or below the smallest, counts the same thresholds as its saturated value, so the counts are exact
// and the search pipeline stays TW + 1 bits wide whatever the accumulator fields.
// Writes are taken only while held: CTRL.HOLD keeps the next frame back (no unit is taken at a frame boundary), and
// STATUS.HELD rises once no frame is in progress (at a frame boundary, the pipeline empty); any other parameter write
// is dropped and sets the sticky STATUS.ERR_BUSY. Every parameter reads back as committed, through the compute read
// ports, so only while STATUS.HELD (else 0). Register map (byte offsets; f_astra.finn_gru_params holds the same
// constants):
//   0x0000 ID 0x47520001 | 0x0004 CTRL bit0 HOLD | 0x0008 STATUS bit0 HELD, bit1 IDLE, bit8 ERR_BUSY, bit9 ERR_RANGE,
//   bit10 ERR_ACCESS (write 1 to clear) | 0x000C WRITES accepted parameter writes (a write clears it) |
//   0x001C FEATURES bit 0: the tables are written (AXIL_TABLES) |
//   0x0010 {BG, BH, H[15:0]} | 0x0014 {AXIL_AW, TW, HAW, XW} | 0x0018 {SH_MIN, SDW, OFW, MULW} (8 bits each)
//   0x1000 + 16 (r 2^clog2(H) + j): row group r (0..5: x r, x z, x n, h r, h z, h n), unit j (contract gate row
//   r H + j): +0 multiplier (staged), +4 offset bits 31:0 (staged), +8 offset bits 63:32 (staged), +12 shift (the
//   whole shift, not the field; commits the row with the staged multiplier and offset)
//   0x4000 + 4 k: sigmoid threshold k = 1 .. 2^BG - 1 (signed) | TANH_BASE + 4 k: tanh threshold k = 1 .. 2^BH - 1,
//   TANH_BASE = 2^max(15, BH + 2)
// Every response is OKAY; partial strobes, read-only or unmapped addresses set ERR_ACCESS.
`timescale 1ns/1ps
module f_astra_gru_delay #(
    parameter integer W = 1,
    parameter integer N = 1
)(
    input  wire clk,
    input  wire ce,
    input  wire [W-1:0] d,
    output wire [W-1:0] q
);
    generate if (N == 0) begin : g_wire
        assign q = d;
    end else begin : g_shift
        reg [W-1:0] r [0:N-1];
        integer i;
        always @(posedge clk) if (ce) begin
            r[0] <= d;
            for (i = 1; i < N; i = i + 1) r[i] <= r[i-1];
        end
        assign q = r[N-1];
    end endgenerate
endmodule

// One step of a binary search over sorted thresholds T[1 .. 2^B - 1]: count bit BIT of #(T <= v). The slice file
// holds T[m * 2^(BIT+1) + 2^BIT] for m < 2^(B-1-BIT); two clocks.
// WR = 1 (the cell's AXIL mode): the slice is a RAM. w_en writes threshold w_index (1-based) if it lies in this slice;
// r_en reads threshold r_index instead of the search's (the cell holds no frame then), r_data one clock later.
(* use_dsp = "no" *)
module f_astra_gru_search_step #(
    parameter integer B = 8,
    parameter integer BIT = 7,
    parameter integer VW = 32,          // value width (signed), at least TW
    parameter integer TW = 16,          // threshold width (signed)
    parameter PATH = "",
    parameter integer BLOCK = 0,        // 1: the slice in block RAM
    parameter integer WR = 0            // 1: write port and read override
)(
    input  wire clk,
    input  wire ce,
    input  wire signed [VW-1:0] v_in,
    input  wire [B-1:0] c_in,
    output reg  signed [VW-1:0] v_out,
    output reg  [B-1:0] c_out,
    input  wire w_en,
    input  wire [B-1:0] w_index,
    input  wire [TW-1:0] w_data,
    input  wire r_en,
    input  wire [B-1:0] r_index,
    output wire [TW-1:0] r_data
);
    localparam integer AB = B - 1 - BIT;
    localparam integer DEPTH = 1 << AB;
    reg [TW-1:0] t;
    reg signed [VW-1:0] v1;
    reg [B-1:0] c1;
    // threshold k is in this slice when k = m 2^(BIT+1) + 2^BIT
    wire w_mine = w_en && (w_index[BIT:0] == (1 << BIT));
    generate if (WR == 0 && AB == 0) begin : g_one
        reg [TW-1:0] rom [0:0];
        initial $readmemh($sformatf("%s%0d.mem", PATH, BIT), rom);
        always @(posedge clk) if (ce) t <= rom[0];
    end else if (WR == 0 && BLOCK) begin : g_block
        (* rom_style = "block" *) reg [TW-1:0] rom [0:DEPTH-1];
        initial $readmemh($sformatf("%s%0d.mem", PATH, BIT), rom);
        always @(posedge clk) if (ce) t <= rom[c_in[B-1:BIT+1]];
    end else if (WR == 0) begin : g_lut
        (* rom_style = "distributed" *) reg [TW-1:0] rom [0:DEPTH-1];
        initial $readmemh($sformatf("%s%0d.mem", PATH, BIT), rom);
        always @(posedge clk) if (ce) t <= rom[c_in[B-1:BIT+1]];
    end else if (AB == 0) begin : g_one_w
        reg [TW-1:0] mem [0:0];
        initial $readmemh($sformatf("%s%0d.mem", PATH, BIT), mem);
        always @(posedge clk) if (w_mine) mem[0] <= w_data;
        always @(posedge clk) if (ce) t <= mem[0];
    end else if (BLOCK) begin : g_block_w
        (* ram_style = "block" *) reg [TW-1:0] mem [0:DEPTH-1];
        initial $readmemh($sformatf("%s%0d.mem", PATH, BIT), mem);
        wire [AB-1:0] ra = r_en ? r_index[B-1:BIT+1] : c_in[B-1:BIT+1];
        always @(posedge clk) if (w_mine) mem[w_index[B-1:BIT+1]] <= w_data;
        always @(posedge clk) if (ce) t <= mem[ra];
    end else begin : g_lut_w
        (* ram_style = "distributed" *) reg [TW-1:0] mem [0:DEPTH-1];
        initial $readmemh($sformatf("%s%0d.mem", PATH, BIT), mem);
        wire [AB-1:0] ra = r_en ? r_index[B-1:BIT+1] : c_in[B-1:BIT+1];
        always @(posedge clk) if (w_mine) mem[w_index[B-1:BIT+1]] <= w_data;
        always @(posedge clk) if (ce) t <= mem[ra];
    end endgenerate
    assign r_data = (WR != 0) ? t : {TW{1'b0}};
    always @(posedge clk) if (ce) begin
        v1 <= v_in;
        c1 <= c_in;
        v_out <= v1;
        c_out <= (v1 >= $signed(t)) ? (c1 | (1 << BIT)) : c1;     // both signed: t is sign-extended
    end
endmodule

// #(T <= v) for 2^B - 1 sorted thresholds: B search steps, 2 B clocks.
// IVW > VW: the input is saturated to VW signed bits first (exact when VW > TW: every threshold lies inside).
// WR = 1: write port and read override of every slice; r_data is the threshold r_index (1-based) read by r_en.
(* use_dsp = "no" *)
module f_astra_gru_search #(
    parameter integer B = 8,
    parameter integer VW = 32,
    parameter integer TW = 16,
    parameter PATH = "",
    parameter integer BLOCK_MIN = 512,  // slices of at least this many thresholds in block RAM
    parameter integer IVW = 0,          // input width; 0: VW
    parameter integer WR = 0
)(
    input  wire clk,
    input  wire ce,
    input  wire signed [((IVW > VW) ? IVW : VW)-1:0] v,
    output wire [B-1:0] count,
    input  wire w_en,
    input  wire [B-1:0] w_index,
    input  wire [TW-1:0] w_data,
    input  wire r_en,
    input  wire [B-1:0] r_index,
    output wire [TW-1:0] r_data
);
    localparam integer IW_ = (IVW > VW) ? IVW : VW;
    wire signed [VW-1:0] vs [0:B];
    wire [B-1:0] cs [0:B];
    wire [TW-1:0] rd [0:B-1];
    wire [TW-1:0] ro [0:B];
    generate if (IW_ > VW) begin : g_sat
        wire over = !v[IW_-1] && (|v[IW_-2:VW-1]);
        wire under = v[IW_-1] && !(&v[IW_-2:VW-1]);
        assign vs[0] = over ? {1'b0, {(VW-1){1'b1}}} : under ? {1'b1, {(VW-1){1'b0}}} : v[VW-1:0];
    end else begin : g_pass
        assign vs[0] = v;
    end endgenerate
    assign cs[0] = {B{1'b0}};
    assign ro[0] = {TW{1'b0}};
    genvar k;
    generate for (k = 0; k < B; k = k + 1) begin : g_step
        f_astra_gru_search_step #(.B(B), .BIT(B - 1 - k), .VW(VW), .TW(TW), .PATH(PATH),
                                  .BLOCK(((1 << k) >= BLOCK_MIN) ? 1 : 0), .WR(WR)) step (
            .clk(clk), .ce(ce), .v_in(vs[k]), .c_in(cs[k]), .v_out(vs[k+1]), .c_out(cs[k+1]),
            .w_en(w_en), .w_index(w_index), .w_data(w_data), .r_en(r_en), .r_index(r_index), .r_data(rd[k]));
        assign ro[k+1] = ro[k] | ((r_index[B-1-k:0] == (1 << (B - 1 - k))) ? rd[k] : {TW{1'b0}});
    end endgenerate
    assign count = cs[B];
    assign r_data = ro[B];
endmodule

// A = rne((acc * mul + add) / 2^(SH_MIN + sh)): three clocks (two partial products, the sum, the rounding shift).
(* use_dsp = "no" *)
module f_astra_gru_requant #(
    parameter integer IW = 26,          // accumulator width (signed)
    parameter integer MULW = 15,        // multiplier width (unsigned), at least 9
    parameter integer OFW = 35,         // offset width (signed)
    parameter integer SDW = 4,          // shift field width: shift = SH_MIN + field
    parameter integer SH_MIN = 14,      // at least 2
    parameter integer OW = 30           // result width (signed)
)(
    input  wire clk,
    input  wire ce,
    input  wire signed [IW-1:0] acc,
    input  wire [MULW-1:0] mul,
    input  wire signed [OFW-1:0] add,
    input  wire [SDW-1:0] sh,
    output reg  signed [OW-1:0] a
);
    localparam integer LO = 8;
    localparam integer HI = MULW - LO;
    localparam integer PW = IW + MULW + 1;
    localparam integer VW = ((PW > OFW) ? PW : OFW) + 1;
    localparam integer LOWB = SH_MIN - 1;
    localparam integer SPAN = 1 << SDW;
    reg signed [IW+LO:0] p_lo;
    reg signed [IW+HI:0] p_hi;
    reg signed [OFW-1:0] add1;
    reg [SDW-1:0] sh1, sh2;
    reg signed [VW-1:0] v2;
    always @(posedge clk) if (ce) begin
        p_lo <= acc * $signed({1'b0, mul[LO-1:0]});
        p_hi <= acc * $signed({1'b0, mul[MULW-1:LO]});
        add1 <= add;
        sh1 <= sh;
        v2 <= (p_hi <<< LO) + p_lo + add1;
        sh2 <= sh1;
    end
    // shift = SH_MIN + sh2: the bits below the round bit are v2[LOWB-1:0] and vs[sh2-1:0]
    wire signed [VW-1:0] vs = v2 >>> LOWB;
    wire signed [VW-1:0] q = vs >>> ({1'b0, sh2} + 1'b1);
    wire round_bit = vs[sh2];
    wire [SPAN-1:0] below = vs[SPAN-1:0] & ((1 << sh2) - 1);
    wire sticky = (|v2[LOWB-1:0]) | (|below);
    wire up = round_bit & (sticky | q[0]);
    always @(posedge clk) if (ce) a <= q + $signed({1'b0, up});
endmodule

(* use_dsp = "no" *)
module f_astra_int_gru_cell #(
    parameter integer H = 64,           // hidden units per frame
    parameter integer XW = 25,          // x-part accumulator width (signed)
    parameter integer HAW = 26,         // hidden-part accumulator width (signed)
    parameter integer BH = 12,          // hidden code width (signed)
    parameter integer BG = 8,           // gate code bits, at least 2
    parameter integer MULW = 15,        // requantization multiplier width (unsigned)
    parameter integer OFW = 35,         // requantization offset width (signed)
    parameter integer SDW = 4,          // shift field width
    parameter integer SH_MIN = 14,      // smallest requantization shift, at least 2
    parameter integer TW = 16,          // sigmoid and tanh threshold width (signed)
    parameter integer HQ = 128,         // hidden-code queue (two frames)
    parameter integer BLOCK_MIN = 512,  // search slices of at least this many thresholds in block RAM
    parameter PARAM_FILE = "gru_requant.mem",   // H words: rows 0..5 at [r*RW +: RW], {shift field, offset, multiplier}
    parameter SIG_PATH = "gru_sigmoid_",
    parameter TANH_PATH = "gru_tanh_",
    parameter integer AXIL = 0,         // 1: AXI4-Lite parameter port (requantization rows, sigmoid and tanh thresholds)
    parameter integer AXIL_TABLES = 1,  // AXIL: 1 the sigmoid and tanh tables written too; 0 fixed (synthesis) tables:
                                        // they follow from the gate, hidden and fixed-point bits alone, so a retrained
                                        // contract keeps them; their addresses are unmapped (FEATURES bit 0 = 0)
    parameter PARAM_PATH = "gru_requant_",      // AXIL: row group r in {PARAM_PATH}{r}.mem, H words {shift field, offset, multiplier}
    parameter integer AXIL_AW = 16      // AXIL: byte address bits, at least max(15, BH + 2) + 1
)(
    input  wire clk,
    input  wire rst_n,
    input  wire x_valid,
    output wire x_ready,
    input  wire [3*XW-1:0] x_data,
    input  wire a_valid,
    output wire a_ready,
    input  wire [3*HAW-1:0] a_data,
    input  wire h_valid,
    output wire h_ready,
    input  wire [BH-1:0] h_data,
    output wire m_valid,
    input  wire m_ready,
    output wire [BH-1:0] m_data,
    input  wire [AXIL_AW-1:0] s_axil_awaddr,       // parameter port (AXIL; unused and constant otherwise)
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
    localparam integer ACCW = (XW > HAW) ? XW : HAW;
    localparam integer PW = ACCW + MULW + 1;
    localparam integer VW = ((PW > OFW) ? PW : OFW) + 1;
    localparam integer AW = VW - SH_MIN + 1;              // requantized value width
    localparam integer RW = SDW + OFW + MULW;              // one row's parameters
    localparam integer RZW = AW + 1;
    localparam integer GL = BG / 2;
    localparam integer MW_ = AW + BG + 1;                  // g * A
    localparam integer PREW = AW + 2;
    // search value widths; AXIL: saturated to TW + 1 bits (exact, see the header)
    localparam integer SVW = (AXIL != 0) ? TW + 1 : ((RZW > TW) ? RZW : TW);
    localparam integer TVW = (AXIL != 0) ? TW + 1 : ((PREW > TW) ? PREW : TW);
    localparam integer LAT = 10 + 2 * BG + 2 * BH;
    localparam integer UW = (H > 1) ? $clog2(H) : 1;
    localparam integer QW = $clog2(HQ);
    localparam signed [BH+2:0] HMAX = (1 <<< (BH - 1)) - 1;
    localparam signed [BH+2:0] HMIN = -(1 <<< (BH - 1));

    reg [LAT-1:0] valid;
    wire ce = !valid[LAT-1] || m_ready;
    assign m_valid = valid[LAT-1];

    // hidden-code queue (distributed RAM, read ahead)
    reg [BH-1:0] hq [0:HQ-1];
    reg [QW-1:0] hq_wr, hq_rd;
    reg [QW:0] hq_count;
    reg [UW-1:0] unit;
    wire hold;                          // AXIL: CTRL.HOLD keeps the next frame back (no unit taken at a frame boundary)
    wire admit = (AXIL == 0) || !(hold && unit == {UW{1'b0}});
    assign h_ready = hq_count < HQ;
    wire hq_put = h_valid && h_ready;
    wire hq_any = hq_count != 0;
    wire take = ce && x_valid && a_valid && hq_any && admit;
    assign x_ready = ce && a_valid && hq_any && admit;
    assign a_ready = ce && x_valid && hq_any && admit;
    always @(posedge clk) if (hq_put) hq[hq_wr] <= h_data;

    always @(posedge clk) begin
        if (!rst_n) begin
            valid <= {LAT{1'b0}};
            hq_wr <= {QW{1'b0}}; hq_rd <= {QW{1'b0}}; hq_count <= {(QW+1){1'b0}};
            unit <= {UW{1'b0}};
        end else begin
            if (ce) valid <= {valid[LAT-2:0], take};
            if (hq_put) hq_wr <= hq_wr + 1'b1;
            if (take) begin
                hq_rd <= hq_rd + 1'b1;
                unit <= (unit == H - 1) ? {UW{1'b0}} : unit + 1'b1;
            end
            hq_count <= hq_count + hq_put - take;
        end
    end

    // stage 1: the unit's accumulators, hidden code and row parameters
    reg [3*XW-1:0] x1;
    reg [3*HAW-1:0] a1;
    reg [BH-1:0] h1;
    wire [6*RW-1:0] prm1;
    always @(posedge clk) if (ce) begin
        x1 <= x_data;
        a1 <= a_data;
        h1 <= hq[hq_rd];
    end
    // AXIL: the parameter RAMs' write port and read override (held: the read port serves AXI reads)
    wire prm_we;
    wire [2:0] prm_wr;
    wire [UW-1:0] prm_wj;
    wire [RW-1:0] prm_wd;
    wire prm_read;
    wire [UW-1:0] prm_unit;
    genvar r;
    generate if (AXIL == 0) begin : g_prm_rom
        (* rom_style = "distributed" *) reg [6*RW-1:0] prm [0:H-1];
        initial $readmemh(PARAM_FILE, prm);
        reg [6*RW-1:0] q;
        always @(posedge clk) if (ce) q <= prm[unit];
        assign prm1 = q;
    end else begin : g_prm_ram
        for (r = 0; r < 6; r = r + 1) begin : g_group
            (* ram_style = "distributed" *) reg [RW-1:0] mem [0:H-1];
            initial $readmemh($sformatf("%s%0d.mem", PARAM_PATH, r), mem);
            reg [RW-1:0] q;
            always @(posedge clk) if (prm_we && prm_wr == r) mem[prm_wj] <= prm_wd;
            always @(posedge clk) if (ce) q <= mem[prm_read ? prm_unit : unit];
            assign prm1[r*RW +: RW] = q;
        end
    end endgenerate

    // stages 2-4: the six requantized rows (x r, x z, x n, h r, h z, h n)
    wire signed [AW-1:0] A [0:5];
    generate for (r = 0; r < 6; r = r + 1) begin : g_row
        wire signed [ACCW-1:0] acc;
        if (r < 3) begin : g_x
            assign acc = $signed(x1[r*XW +: XW]);
        end else begin : g_h
            assign acc = $signed(a1[(r-3)*HAW +: HAW]);
        end
        f_astra_gru_requant #(.IW(ACCW), .MULW(MULW), .OFW(OFW), .SDW(SDW), .SH_MIN(SH_MIN), .OW(AW)) rq (
            .clk(clk), .ce(ce), .acc(acc), .mul(prm1[r*RW +: MULW]), .add($signed(prm1[r*RW + MULW +: OFW])),
            .sh(prm1[r*RW + MULW + OFW +: SDW]), .a(A[r]));
    end endgenerate

    // stage 5: gate pre-activations
    reg signed [RZW-1:0] rz_r5, rz_z5;
    reg signed [AW-1:0] anx5, anh5;
    always @(posedge clk) if (ce) begin
        rz_r5 <= A[0] + A[3];
        rz_z5 <= A[1] + A[4];
        anx5 <= A[2];
        anh5 <= A[5];
    end

    // AXIL: threshold write ports and read overrides
    wire sig_we, tanh_we, tab_read;
    wire [BG-1:0] sig_wk, sig_rk;
    wire [BH-1:0] tanh_wk, tanh_rk;
    wire [TW-1:0] tab_wd, sig_rd, tanh_rd;
    localparam integer WRP = (AXIL != 0 && AXIL_TABLES != 0) ? 1 : 0;

    // stages 6 .. 5 + 2 BG: sigmoid threshold counts
    wire [BG-1:0] g_r, g_z;
    f_astra_gru_search #(.B(BG), .VW(SVW), .TW(TW), .PATH(SIG_PATH), .BLOCK_MIN(BLOCK_MIN), .IVW(RZW), .WR(WRP)) sig_r (
        .clk(clk), .ce(ce), .v(rz_r5), .count(g_r), .w_en(sig_we), .w_index(sig_wk), .w_data(tab_wd), .r_en(tab_read),
        .r_index(sig_rk), .r_data(sig_rd));
    f_astra_gru_search #(.B(BG), .VW(SVW), .TW(TW), .PATH(SIG_PATH), .BLOCK_MIN(BLOCK_MIN), .IVW(RZW), .WR(WRP)) sig_z (
        .clk(clk), .ce(ce), .v(rz_z5), .count(g_z), .w_en(sig_we), .w_index(sig_wk), .w_data(tab_wd), .r_en(tab_read),
        .r_index(sig_rk), .r_data());
    wire [AW-1:0] anh_s, anx_s;
    f_astra_gru_delay #(.W(AW), .N(2 * BG)) d_anh (.clk(clk), .ce(ce), .d(anh5), .q(anh_s));
    f_astra_gru_delay #(.W(AW), .N(2 * BG + 2)) d_anx (.clk(clk), .ce(ce), .d(anx5), .q(anx_s));

    // stages 6 + 2 BG, 7 + 2 BG: g_r * A_nh; stage 8 + 2 BG: pre = A_nx + rne(g_r * A_nh / 2^BG)
    reg signed [AW+GL:0] gp_lo;
    reg signed [AW+BG-GL:0] gp_hi;
    reg signed [MW_-1:0] gm;
    reg signed [PREW-1:0] pre;
    always @(posedge clk) if (ce) begin
        gp_lo <= $signed(anh_s) * $signed({1'b0, g_r[GL-1:0]});
        gp_hi <= $signed(anh_s) * $signed({1'b0, g_r[BG-1:GL]});
        gm <= (gp_hi <<< GL) + gp_lo;
    end
    wire signed [MW_-1:0] gq = gm >>> BG;
    wire g_up = gm[BG-1] & ((|gm[BG-2:0]) | gq[0]);
    always @(posedge clk) if (ce) pre <= $signed(anx_s) + gq + $signed({1'b0, g_up});

    // stages 9 + 2 BG .. 8 + 2 BG + 2 BH: tanh threshold count
    wire [BH-1:0] c_n;
    f_astra_gru_search #(.B(BH), .VW(TVW), .TW(TW), .PATH(TANH_PATH), .BLOCK_MIN(BLOCK_MIN), .IVW(PREW), .WR(WRP)) tanh (
        .clk(clk), .ce(ce), .v(pre), .count(c_n), .w_en(tanh_we), .w_index(tanh_wk), .w_data(tab_wd), .r_en(tab_read),
        .r_index(tanh_rk), .r_data(tanh_rd));
    wire [BG-1:0] g_z_s;
    wire [BH-1:0] h_s;
    f_astra_gru_delay #(.W(BG), .N(3 + 2 * BH)) d_gz (.clk(clk), .ce(ce), .d(g_z), .q(g_z_s));
    f_astra_gru_delay #(.W(BH), .N(LAT - 3)) d_h (.clk(clk), .ce(ce), .d(h1), .q(h_s));

    // stage LAT - 1: n and g_z * (h - n); stage LAT: h' = clip(n + rne(g_z * (h - n) / 2^BG))
    wire signed [BH:0] n = $signed({1'b0, c_n}) - $signed({1'b0, 1'b1, {(BH-1){1'b0}}});
    wire signed [BH+1:0] diff = $signed(h_s) - n;
    reg signed [BH+BG+2:0] zm;
    reg signed [BH:0] n9;
    always @(posedge clk) if (ce) begin
        zm <= diff * $signed({1'b0, g_z_s});
        n9 <= n;
    end
    wire signed [BH+BG+2:0] zq = zm >>> BG;
    wire z_up = zm[BG-1] & ((|zm[BG-2:0]) | zq[0]);
    wire signed [BH+2:0] hn = n9 + zq + $signed({1'b0, z_up});
    reg [BH-1:0] out;
    always @(posedge clk) if (ce) out <= (hn > HMAX) ? HMAX[BH-1:0] : (hn < HMIN) ? HMIN[BH-1:0] : hn[BH-1:0];
    assign m_data = out;

    // ------------------------------------------------------------------ run-time parameters (AXIL)
    // No frame in progress: at a frame boundary (the next beat is unit 0) and the pipeline empty.
    wire frame_idle = (unit == {UW{1'b0}}) && (valid == {LAT{1'b0}});
    generate if (AXIL != 0) begin : params
        localparam integer ROWS = 'h1000, SIGS = 'h4000;
        localparam integer TB = (BH + 2 > 15) ? BH + 2 : 15;
        localparam integer TANHS = 1 << TB;
        localparam integer RB = 4 + UW;                                 // row offset bits inside a row group
        localparam integer SH_MAX = SH_MIN + (1 << SDW) - 1;
        localparam [31:0] ID = 32'h47520001;
        reg hold_r, err_busy, err_range, err_access;
        reg [31:0] writes;
        reg aw_full, w_full, ar_full, rd_wait, bvalid, rvalid;
        reg [AXIL_AW-1:0] aw_addr, ar_addr;
        reg [31:0] w_data, rdata, mul_stage, off_lo, off_hi;
        reg [3:0] w_strb;
        reg rd_ok;                                                      // the parameter read was taken while held

        wire held = hold_r && frame_idle;
        wire [31:0] status = {21'd0, err_access, err_range, err_busy, 6'd0, frame_idle, held};
        // write decode (registered address and data)
        wire w_full_strb = (w_strb == 4'hF);
        wire w_ctrl = aw_addr < ROWS;
        wire [AXIL_AW-1:0] w_rel = aw_addr - ROWS;
        wire [2:0] w_r = w_rel[RB +: 3];
        wire [UW-1:0] w_j = w_rel[4 +: UW];
        wire w_row = !w_ctrl && (aw_addr < SIGS) && ((w_rel >> (RB + 3)) == 0) && (w_r < 6) && (w_j < H);
        wire [BG-1:0] w_sk = aw_addr[2 +: BG];
        wire [BH-1:0] w_tk = aw_addr[2 +: BH];
        wire w_sig = (AXIL_TABLES != 0) && (aw_addr >= SIGS) && (aw_addr < SIGS + 4 * (1 << BG)) && (w_sk != 0);
        wire w_tanh = (AXIL_TABLES != 0) && (aw_addr >= TANHS) && (aw_addr < TANHS + 4 * (1 << BH)) && (w_tk != 0);
        wire w_param = w_row || w_sig || w_tanh;
        wire [63:0] off_new = {off_hi, off_lo};
        wire [31:0] sh_rel = w_data - SH_MIN;
        wire row_ok = (mul_stage >> MULW) == 0 && ((&off_new[63:OFW-1]) || !(|off_new[63:OFW-1]))
                      && w_data >= SH_MIN && w_data <= SH_MAX;
        wire tab_ok = (&w_data[31:TW-1]) || !(|w_data[31:TW-1]);       // a sign-extended TW-bit value
        wire w_commit = w_row && aw_addr[3:2] == 2'd3;
        wire value_ok = w_commit ? row_ok : (w_sig || w_tanh) ? tab_ok : 1'b1;
        wire do_write = aw_full && w_full && !bvalid;
        wire accept = do_write && w_full_strb && !w_ctrl && w_param && held && value_ok;
        // read decode
        wire r_ctrl = ar_addr < ROWS;
        wire [AXIL_AW-1:0] r_rel = ar_addr - ROWS;
        wire [2:0] r_r = r_rel[RB +: 3];
        wire [UW-1:0] r_j = r_rel[4 +: UW];
        wire r_row = !r_ctrl && (ar_addr < SIGS) && ((r_rel >> (RB + 3)) == 0) && (r_r < 6) && (r_j < H);
        wire [BG-1:0] r_sk = ar_addr[2 +: BG];
        wire [BH-1:0] r_tk = ar_addr[2 +: BH];
        wire r_sig = (AXIL_TABLES != 0) && (ar_addr >= SIGS) && (ar_addr < SIGS + 4 * (1 << BG)) && (r_sk != 0);
        wire r_tanh = (AXIL_TABLES != 0) && (ar_addr >= TANHS) && (ar_addr < TANHS + 4 * (1 << BH)) && (r_tk != 0);
        wire [RW-1:0] r_fields = prm1[r_r * RW +: RW];
        wire [OFW-1:0] r_off = r_fields[MULW +: OFW];
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
                    10'd4: read_value = {BG[7:0], BH[7:0], H[15:0]};
                    10'd5: read_value = {AXIL_AW[7:0], TW[7:0], HAW[7:0], XW[7:0]};
                    10'd6: read_value = {SH_MIN[7:0], SDW[7:0], OFW[7:0], MULW[7:0]};
                    10'd7: read_value = {31'd0, AXIL_TABLES != 0};
                    default: read_value = 32'd0;
                endcase
            end else if (rd_ok) begin
                if (r_row) case (ar_addr[3:2])
                    2'd0: read_value = r_fields[MULW-1:0];
                    2'd1: read_value = r_off64[31:0];
                    2'd2: read_value = r_off64[63:32];
                    default: read_value = SH_MIN + r_fields[MULW + OFW +: SDW];
                endcase
                else if (r_sig) read_value = {{(32 - TW){sig_rd[TW-1]}}, sig_rd};
                else if (r_tanh) read_value = {{(32 - TW){tanh_rd[TW-1]}}, tanh_rd};
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
                    else if (w_ctrl) begin
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
                        if (w_row && aw_addr[3:2] == 2'd0) mul_stage <= w_data;
                        if (w_row && aw_addr[3:2] == 2'd1) off_lo <= w_data;
                        if (w_row && aw_addr[3:2] == 2'd2) off_hi <= w_data;
                    end
                end
                // a read takes the ports when no write does; the data is there one cycle later
                if (ar_full && !rvalid && !rd_wait && !do_write) begin rd_wait <= 1'b1; rd_ok <= held; end
                if (rd_wait) begin
                    rd_wait <= 1'b0; ar_full <= 1'b0; rvalid <= 1'b1; rdata <= read_value;
                end
            end
        end
        // the commit of a row (its shift write) and the threshold writes reach the RAMs' write ports
        assign prm_we = accept && w_commit;
        assign prm_wr = w_r;
        assign prm_wj = w_j;
        assign prm_wd = {sh_rel[SDW-1:0], off_new[OFW-1:0], mul_stage[MULW-1:0]};
        assign sig_we = accept && w_sig;
        assign tanh_we = accept && w_tanh;
        assign sig_wk = w_sk;
        assign tanh_wk = w_tk;
        assign tab_wd = w_data[TW-1:0];
        assign prm_read = held;
        assign prm_unit = r_j;
        assign tab_read = held;
        assign sig_rk = r_sk;
        assign tanh_rk = r_tk;
        assign hold = hold_r;
        assign s_axil_awready = rst_n && !aw_full;
        assign s_axil_wready = rst_n && !w_full;
        assign s_axil_bresp = 2'b00;
        assign s_axil_bvalid = bvalid;
        assign s_axil_arready = rst_n && !ar_full;
        assign s_axil_rdata = rdata;
        assign s_axil_rresp = 2'b00;
        assign s_axil_rvalid = rvalid;
    end else begin : fixed
        assign prm_we = 1'b0;
        assign prm_wr = 3'd0;
        assign prm_wj = {UW{1'b0}};
        assign prm_wd = {RW{1'b0}};
        assign sig_we = 1'b0;
        assign tanh_we = 1'b0;
        assign sig_wk = {BG{1'b0}};
        assign tanh_wk = {BH{1'b0}};
        assign tab_wd = {TW{1'b0}};
        assign prm_read = 1'b0;
        assign prm_unit = {UW{1'b0}};
        assign tab_read = 1'b0;
        assign sig_rk = {BG{1'b0}};
        assign tanh_rk = {BH{1'b0}};
        assign hold = 1'b0;
        assign s_axil_awready = 1'b0;
        assign s_axil_wready = 1'b0;
        assign s_axil_bresp = 2'b00;
        assign s_axil_bvalid = 1'b0;
        assign s_axil_arready = 1'b0;
        assign s_axil_rdata = 32'd0;
        assign s_axil_rresp = 2'b00;
        assign s_axil_rvalid = 1'b0;
    end endgenerate
endmodule
