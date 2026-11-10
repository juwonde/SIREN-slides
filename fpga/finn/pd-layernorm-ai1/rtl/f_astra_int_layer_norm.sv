// F-ASTRA integer LayerNorm: integer contract v3 kind layer_norm (integer_contract.layer_norm) over C channels.
//   mean = sign(sum) * RNE(|sum| / C); c = x - mean; variance = RNE(sum(c^2) / C);
//   root = floor(sqrt(variance + EPS)); n = sign(c) * RNE((|c| << 26) / root), saturated to int32;
//   out = RNE((n * gamma + (beta << 26)) >> (56 - OQ)), saturated to OW bits. Ties go to even.
// One row of C codes in, one row out. Two row banks: the next row comes in while the current row is
// processed (mean -> variance -> root -> reciprocal, then the normalized replay). One pass over the row: the sum
// and the sum of squares S2 = sum(x^2) accumulate while the row comes in, and sum(c^2) = S2 - 2 * mean * sum +
// C * mean^2 holds exactly in integers (c = x - mean), so the variance needs no second read of the row bank and a
// row takes about C + 200 cycles instead of 2C + 200.
// The division by root is exact: q = floor(N * floor(2^P / root) / 2^P) is at most one below N / root
// because N < 2^P, so one remainder correction gives the floor and the remainder gives the RNE step.
// Coefficient sets (FINN loop rolling): with SETS > 1 the ROM holds SETS * C words, set s at [s*C, (s+1)*C).
// One set-index beat (i_data, below SETS) is taken before the first row of every frame of ROWS rows; each
// row bank keeps the set of its frame and the normalized replay reads word set * C + channel. With SETS = 1
// the index port is unused (i_ready stays low) and the core is the single-set design.
// Per-set output point and epsilon (SETS > 1): SET_OQ holds each set's OQ (8-bit two's complement, set s at
// [8s +: 8]) and SET_EPS its epsilon ([64s +: 64]); both default to OQ and EPS for every set. A row latches its
// set's epsilon and pre-shift OQ_s - OQ_LO with the coefficient base; the affine sum is shifted left by it, so the
// output stage rounds at the fixed shift 56 - OQ_LO, which equals RNE(sum >> (56 - OQ_s)) exactly (floor, remainder
// and half all scale by the same power of two). ROOT_LOG2 must hold for the smallest epsilon of all sets.
// Run-time parameters (AXIL = 1, SETS > 1 only): an AXI4-Lite slave (32-bit data, AXIL_AW address bits) writes every
// set's gamma and beta (the coefficient memory becomes a RAM), output point and epsilon. Power-on contents are the
// synthesis values (COEF_FILE, SET_OQ, SET_EPS); ap_rst_n does not change them (it clears HOLD, the sticky errors and
// WRITES only), so written parameters survive an accelerator soft reset. Every parameter reads back as committed
// (gamma and beta only while STATUS.HELD: they use the compute read port, else they read 0). The
// output point window OQ_MIN .. OQ_MAX and the epsilon floor 4^ROOT_LOG2 are synthesis fields: a value outside them is
// refused, never narrowed.
// Writes are taken only while held: CTRL.HOLD keeps the next frame's set index back, and STATUS.HELD rises once no
// frame is in progress (no set held, both row banks empty, statistics idle); any other parameter write is dropped and
// sets the sticky STATUS.ERR_BUSY. Register map (byte offsets; f_astra.finn_ln_params holds the same constants):
//   0x0000 ID 0x4C4E0001 | 0x0004 CTRL bit0 HOLD | 0x0008 STATUS bit0 HELD, bit1 IDLE, bit8 ERR_BUSY, bit9 ERR_RANGE,
//   bit10 ERR_ACCESS (write 1 to clear) | 0x000C WRITES accepted parameter writes (a write clears it) |
//   0x0010 {ROWS[15:0], C[15:0]} | 0x0014 {ROOT_LOG2, OQ_MAX, OQ_MIN, SETS - 1} (8 bits each) | 0x0018 {AXIL_AW, OW, IW}
//   0x1000 + 16 s: +0 output point of set s (signed), +8 epsilon bits 31:0 (staged), +12 epsilon bits 63:32 (commits
//   the staged low word) | 0x2000 + 8 (s C + c): +0 gamma (staged), +4 beta (commits {beta, the staged gamma}) of
//   set s, channel c (signed 32-bit Q30); write gamma, then beta of the same channel. One 64-bit write per channel
//   and reads through the compute port keep the coefficient RAM simple dual-port with one write enable.
// Every response is OKAY; partial strobes, read-only or unmapped addresses set ERR_ACCESS. AXIL adds one register
// stage for the pre-shift (one cycle more latency per row).
`timescale 1ns/1ps
module f_astra_seq_divide #(
    parameter integer NW = 64,          // dividend and quotient width
    parameter integer DW = 32           // divisor width
)(
    input  wire clk,
    input  wire start,
    input  wire [NW-1:0] dividend,
    input  wire [DW-1:0] divisor,
    output reg  done,
    output reg  [NW-1:0] quotient,
    output reg  [DW-1:0] remainder
);
    reg busy;
    reg [$clog2(NW + 1)-1:0] count;
    reg [NW-1:0] q;
    reg [DW:0] r;
    wire [DW+1:0] shifted = {r, q[NW-1]};
    wire take = shifted >= {2'b00, divisor};
    wire [DW+1:0] reduced = shifted - {2'b00, divisor};
    always @(posedge clk) begin
        done <= 1'b0;
        if (start) begin
            busy <= 1'b1; count <= NW; q <= dividend; r <= {(DW+1){1'b0}};
        end else if (busy) begin
            r <= take ? reduced[DW:0] : shifted[DW:0];
            q <= {q[NW-2:0], take};
            count <= count - 1'b1;
            if (count == 1) begin
                busy <= 1'b0; done <= 1'b1;
            end
        end
        if (done) begin
            quotient <= q; remainder <= r[DW-1:0];
        end
    end
    initial begin busy = 1'b0; done = 1'b0; end
endmodule

module f_astra_seq_isqrt #(
    parameter integer W = 64            // even input width
)(
    input  wire clk,
    input  wire start,
    input  wire [W-1:0] value,
    output reg  done,
    output reg  [W/2-1:0] root
);
    localparam integer H = W / 2;
    reg busy;
    reg [$clog2(H + 1)-1:0] count;
    reg [W-1:0] v;
    reg [H+1:0] rest;                   // the remainder stays at most 2 * root < 2^(H+1)
    reg [H-1:0] result;
    wire [H+3:0] widened = {rest, v[W-1:W-2]};
    wire [H+3:0] trial = {2'b00, result, 2'b01};
    wire fits = widened >= trial;
    always @(posedge clk) begin
        done <= 1'b0;
        if (start) begin
            busy <= 1'b1; count <= H; v <= value; rest <= {(H+2){1'b0}}; result <= {H{1'b0}};
        end else if (busy) begin
            rest <= fits ? (widened - trial) : widened[H+1:0];
            result <= {result[H-2:0], fits};
            v <= {v[W-3:0], 2'b00};
            count <= count - 1'b1;
            if (count == 1) begin
                busy <= 1'b0; done <= 1'b1;
            end
        end
        if (done) root <= result;
    end
    initial begin busy = 1'b0; done = 1'b0; end
endmodule

module f_astra_int_layer_norm #(
    parameter integer C = 768,          // channels per row
    parameter integer IW = 28,          // input code width (signed), at most 30
    parameter integer OW = 28,          // output code width (signed)
    parameter integer OQ = 22,          // output binary point, 0..56
    parameter [63:0] EPS = 64'd1,       // epsilon at twice the input point, at least one
    parameter integer ROOT_LOG2 = 0,    // floor(log2(floor(sqrt(EPS)))): every root is at least 2^ROOT_LOG2
    parameter integer SETS = 1,         // coefficient sets, 1..256
    parameter integer ROWS = 1,         // rows per frame (one set index per frame), used when SETS > 1
    parameter COEF_FILE = "f_astra_layer_norm_coef.mem",  // SETS * C words {beta[31:0], gamma[31:0]}, Q30
    parameter [8*SETS-1:0] SET_OQ = {SETS{OQ[7:0]}},     // SETS > 1: output point of each set
    parameter [64*SETS-1:0] SET_EPS = {SETS{EPS}},       // SETS > 1: epsilon of each set
    parameter integer AXIL = 0,         // SETS > 1: AXI4-Lite parameter port (gamma, beta, output point, epsilon)
    parameter integer OQ_MIN = 0,       // AXIL: writable output points OQ_MIN .. OQ_MAX, inside -6 .. 56
    parameter integer OQ_MAX = 0,
    parameter integer AXIL_AW = 16      // AXIL: byte address bits, at least clog2(8192 + 8 * SETS * C)
)(
    input  wire clk,
    input  wire rst_n,
    input  wire s_valid,
    output wire s_ready,
    input  wire [IW-1:0] s_data,
    output wire m_valid,
    input  wire m_ready,
    output wire [OW-1:0] m_data,
    input  wire i_valid,                // set index stream (SETS > 1)
    output wire i_ready,
    input  wire [7:0] i_data,
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
    localparam integer AW = (C > 2) ? $clog2(C) : 1;
    localparam integer SW = IW + AW + 1;                 // row sum
    localparam integer CW = IW + 1;                      // centered code
    localparam integer QW = 2 * IW + AW + 1;             // sum of squares (of the codes, and of the centered codes)
    localparam integer VW = 2 * IW + AW + 5;             // signed terms of S2 - 2 * mean * sum + C * mean^2
    localparam integer P = IW + 26;                      // |c| << 26 < 2^P
    localparam integer RW = P - ROOT_LOG2 + 1;           // floor(2^P / root) and the quotient estimate
    localparam integer DIVW = (QW > P + 1) ? QW : P + 1;
    // output points: one with a single set, the range of SET_OQ with several, the writable window with AXIL
    function integer oq_bound(input integer highest);
        integer s, v;
        begin
            oq_bound = (SETS > 1) ? $signed(SET_OQ[7:0]) : OQ;
            for (s = 1; s < SETS; s = s + 1) begin
                v = $signed(SET_OQ[8*s +: 8]);
                if (highest ? (v > oq_bound) : (v < oq_bound)) oq_bound = v;
            end
            if (AXIL != 0) oq_bound = highest ? OQ_MAX : OQ_MIN;
        end
    endfunction
    localparam integer OQ_LO = oq_bound(0);
    localparam integer SPREAD = oq_bound(1) - OQ_LO;    // largest left pre-shift
    localparam integer PW = (SPREAD > 1) ? $clog2(SPREAD + 1) : 1;
    localparam integer EW = 64 + SPREAD;                // affine sum after the pre-shift
    localparam integer SHIFT = 56 - OQ_LO;
    localparam integer SH_ = (SHIFT > 0) ? SHIFT : 1;
    localparam [SH_-1:0] HALF = 1 << (SH_ - 1);
    localparam signed [63:0] OUT_MAX = (64'sd1 <<< (OW - 1)) - 1;
    localparam signed [63:0] OUT_MIN = -(64'sd1 <<< (OW - 1));
    localparam [31:0] CHANNELS = C;
    localparam integer XW = (SETS > 1) ? $clog2(SETS) : 1;          // set number
    localparam integer FW = (ROWS > 1) ? $clog2(ROWS) : 1;          // row of the frame
    localparam integer CD = (SETS > 1) ? SETS * C : (1 << AW);      // coefficient words
    localparam integer CAW = (SETS > 1) ? $clog2(SETS * C) : AW;    // coefficient address
    localparam integer XS = (AXIL != 0) ? 1 : 0;                    // the pre-shift in its own stage (AXIL)
    // power-on pre-shifts OQ_s - OQ_LO of the parameter registers (AXIL)
    function [PW*SETS-1:0] pre_init(input integer unused);
        integer s;
        begin
            pre_init = {(PW*SETS){1'b0}};
            for (s = 0; s < SETS; s = s + 1) pre_init[PW*s +: PW] = $signed(SET_OQ[8*s +: 8]) - OQ_LO;
        end
    endfunction

    // ------------------------------------------------------------------ row banks and coefficients
    (* ram_style = "block" *) reg [IW-1:0] rows [0:(2 << AW) - 1];
    (* rom_style = "block", ram_style = "block" *) reg [63:0] coef [0:CD - 1];   // a RAM with AXIL
    initial $readmemh(COEF_FILE, coef);

    reg wb, rb;                         // bank being written, bank being processed
    reg [1:0] full;
    reg [AW-1:0] wi;
    reg signed [SW-1:0] wsum;
    reg signed [SW-1:0] rowsum0, rowsum1;
    // sum of squares on the way in: x^2 two stages behind the accept; a row's S2 and its full flag land with its
    // last square (two cycles after its last code)
    reg in_v1, in_v2, in_last1, in_last2, in_b1, in_b2;
    reg signed [IW-1:0] in_x1;
    reg [2*IW-1:0] in_p2;
    reg [QW-1:0] wsq, rowsq0, rowsq1;
    // set index: taken once per frame before its first row; each bank keeps the set of the row it holds
    reg set_held;
    reg [XW-1:0] set_cur, set0, set1;
    reg [FW-1:0] wrow;
    reg [CAW-1:0] coef_base, caddr;     // set * C, then the coefficient word of the next issued element
    reg [63:0] eps_set;                 // epsilon and pre-shift of the row's set (SETS > 1)
    reg [PW-1:0] pre_set;
    wire [63:0] eps_row = (SETS > 1) ? eps_set : EPS;
    wire [XW-1:0] row_set = rb ? set1 : set0;                       // set of the bank the statistics take next
    wire hold;                          // AXIL: CTRL.HOLD keeps the next frame's set index back
    wire [63:0] axil_eps;               // AXIL: epsilon and pre-shift of row_set from the parameter registers
    wire [PW-1:0] axil_pre;
    wire axil_coef_read;                // AXIL: held, the coefficient read port serves AXI reads of axil_coef_word
    wire [CAW-1:0] axil_coef_word;
    assign i_ready = rst_n && (SETS > 1) && !set_held && !hold;
    wire take_set = i_valid && i_ready;

    wire accept = s_valid && s_ready;
    assign s_ready = rst_n && !full[wb] && (SETS == 1 || set_held);
    always @(posedge clk) if (accept) rows[{wb, wi}] <= s_data;

    // ------------------------------------------------------------------ statistics
    localparam S_IDLE = 4'd0, S_MEAN = 4'd1, S_NUM1 = 4'd2, S_VAR = 4'd3, S_SQRT = 4'd4, S_RECIP = 4'd5,
               S_EMIT = 4'd6, S_DONE = 4'd7, S_NUM2 = 4'd8;
    reg [3:0] state;
    reg div_start; reg [DIVW-1:0] div_n; reg [31:0] div_d;
    wire div_done; wire [DIVW-1:0] div_q; wire [31:0] div_r;
    f_astra_seq_divide #(.NW(DIVW), .DW(32)) divider(.clk(clk), .start(div_start), .dividend(div_n), .divisor(div_d),
        .done(div_done), .quotient(div_q), .remainder(div_r));
    reg sqrt_start; reg [63:0] sqrt_v; wire sqrt_done; wire [31:0] sqrt_root;
    f_astra_seq_isqrt #(.W(64)) square_root(.clk(clk), .start(sqrt_start), .value(sqrt_v), .done(sqrt_done), .root(sqrt_root));
    reg div_pending, sqrt_pending;      // the result arrives one cycle after done

    reg sum_negative;
    reg signed [CW-1:0] mean;
    reg signed [VW-1:0] num_ms, num_mm;  // mean * sum, mean * mean
    reg [63:0] variance;
    reg [31:0] root;
    reg [RW-1:0] reciprocal;
    wire [DIVW-1:0] rounded_q = div_q + (((({1'b0, div_r} << 1) > {1'b0, CHANNELS}) ||
                                          ((({1'b0, div_r} << 1) == {1'b0, CHANNELS}) && div_q[0])) ? 1'b1 : 1'b0);
    wire [DIVW+63:0] rounded_wide = {64'd0, rounded_q};   // zero extension: DIVW may be below 64

    // normalized replay (stall-all pipeline)
    localparam integer L = 20 + XS;     // read register, then stages e2 .. e20 (and e19s with AXIL)
    reg [L-1:0] ev;
    wire ce = !ev[L-1] || m_ready;
    reg [AW:0] ei, eout;
    wire issue = (state == S_EMIT) && ce && (ei < C);

    wire read_en = ce;
    wire [AW-1:0] read_index = ei[AW-1:0];
    // caddr = set * C + ei in the replay; while held (AXIL, no frame in progress) the AXI read word of the coefficients
    wire [CAW-1:0] coef_index = (AXIL != 0 && axil_coef_read) ? axil_coef_word : (SETS > 1) ? caddr : read_index;
    reg [IW-1:0] row_q;
    reg [63:0] coef_q;
    always @(posedge clk) if (read_en) begin
        row_q <= rows[{rb, read_index}];
        coef_q <= coef[coef_index];
    end

    always @(posedge clk) begin
        div_start <= 1'b0; sqrt_start <= 1'b0;
        if (!rst_n) begin
            wb <= 1'b0; rb <= 1'b0; full <= 2'b00; wi <= {AW{1'b0}}; wsum <= {SW{1'b0}};
            state <= S_IDLE; div_pending <= 1'b0; sqrt_pending <= 1'b0;
            set_held <= 1'b0; wrow <= {FW{1'b0}};
            in_v1 <= 1'b0; in_v2 <= 1'b0; wsq <= {QW{1'b0}};
        end else begin
            // the squares stages: x, x^2, accumulate (the row's last square sets its bank full)
            in_v1 <= accept; in_last1 <= accept && (wi == C - 1); in_b1 <= wb; in_x1 <= s_data;
            in_v2 <= in_v1; in_last2 <= in_last1; in_b2 <= in_b1; in_p2 <= in_x1 * in_x1;
            if (in_v2) begin
                if (in_last2) begin
                    if (in_b2) rowsq1 <= wsq + in_p2; else rowsq0 <= wsq + in_p2;
                    full[in_b2] <= 1'b1;
                    wsq <= {QW{1'b0}};
                end else wsq <= wsq + in_p2;
            end
            if (take_set) begin
                set_cur <= i_data[XW-1:0]; set_held <= 1'b1;
            end
            if (accept) begin
                if (wi == C - 1) begin
                    if (wb) rowsum1 <= wsum + $signed(s_data); else rowsum0 <= wsum + $signed(s_data);
                    if (wb) set1 <= set_cur; else set0 <= set_cur;
                    wsum <= {SW{1'b0}}; wi <= {AW{1'b0}}; wb <= ~wb;
                    if (wrow == ROWS - 1) begin
                        wrow <= {FW{1'b0}}; set_held <= 1'b0;
                    end else wrow <= wrow + 1'b1;
                end else begin
                    wsum <= wsum + $signed(s_data); wi <= wi + 1'b1;
                end
            end
            case (state)
                S_IDLE: if (full[rb]) begin
                    coef_base <= row_set * CHANNELS;
                    if (SETS > 1) eps_set <= (AXIL != 0) ? axil_eps : SET_EPS[64 * row_set +: 64];
                    pre_set <= (SPREAD == 0) ? 0 : (AXIL != 0) ? axil_pre : $signed(SET_OQ[8 * row_set +: 8]) - OQ_LO;
                    sum_negative <= rb ? rowsum1[SW-1] : rowsum0[SW-1];
                    div_n <= rb ? (rowsum1[SW-1] ? -rowsum1 : rowsum1) : (rowsum0[SW-1] ? -rowsum0 : rowsum0);
                    div_d <= CHANNELS; div_start <= 1'b1; state <= S_MEAN;
                end
                S_MEAN: begin
                    if (div_done) div_pending <= 1'b1;
                    if (div_pending) begin
                        div_pending <= 1'b0;
                        mean <= sum_negative ? -$signed({1'b0, rounded_q[CW-2:0]}) : $signed({1'b0, rounded_q[CW-2:0]});
                        state <= S_NUM1;
                    end
                end
                S_NUM1: begin                   // mean * sum and mean * mean (registered products)
                    num_ms <= mean * (rb ? rowsum1 : rowsum0);
                    num_mm <= mean * mean;
                    state <= S_NUM2;
                end
                S_NUM2: begin                   // sum(c^2) = S2 - 2 * mean * sum + C * mean^2 (exact, non-negative)
                    div_n <= $signed({{(VW-QW){1'b0}}, rb ? rowsq1 : rowsq0}) - (num_ms <<< 1)
                             + num_mm * $signed({1'b0, CHANNELS});
                    div_d <= CHANNELS; div_start <= 1'b1; state <= S_VAR;
                end
                S_VAR: begin
                    if (div_done) div_pending <= 1'b1;
                    if (div_pending) begin
                        div_pending <= 1'b0;
                        variance <= rounded_wide[63:0];
                        sqrt_v <= rounded_wide[63:0] + eps_row; sqrt_start <= 1'b1; state <= S_SQRT;
                    end
                end
                S_SQRT: begin
                    if (sqrt_done) sqrt_pending <= 1'b1;
                    if (sqrt_pending) begin
                        sqrt_pending <= 1'b0;
                        root <= sqrt_root;
                        div_n <= {{(DIVW-1){1'b0}}, 1'b1} << P; div_d <= sqrt_root; div_start <= 1'b1; state <= S_RECIP;
                    end
                end
                S_RECIP: begin
                    if (div_done) div_pending <= 1'b1;
                    if (div_pending) begin
                        div_pending <= 1'b0;
                        reciprocal <= div_q[RW-1:0]; ei <= 0; eout <= 0; caddr <= coef_base; state <= S_EMIT;
                    end
                end
                S_EMIT: begin
                    if (issue) begin
                        ei <= ei + 1'b1; caddr <= caddr + 1'b1;
                    end
                    if (m_valid && m_ready) begin
                        eout <= eout + 1'b1;
                        if (eout == C - 1) state <= S_DONE;
                    end
                end
                S_DONE: begin
                    full[rb] <= 1'b0; rb <= ~rb; state <= S_IDLE;
                end
                default: state <= S_IDLE;
            endcase
        end
    end

    // ------------------------------------------------------------------ normalized replay
    // e1 is the registered read (row_q, coef_q). The wide products have registers on both sides so
    // synthesis can place them in the DSP cascade (A/B, M and P registers).
    reg signed [CW-1:0] e2_c;
    reg [P-1:0] e3_n, e4_n, e5_n, e6_n, e7_n, e8_n, e9_n, e10_n, e11_n;
    reg [RW-1:0] e4_r;
    reg [P+RW-1:0] e5_m, e6_m, e7_m;
    reg [RW-1:0] e8_q, e9_q, e10_q, e11_q, e12_q;
    reg [31:0] e8_root;
    reg [RW+31:0] e9_t, e10_t, e11_t;
    reg signed [P+1:0] e12_r;
    reg [RW:0] e13_q; reg signed [P+1:0] e13_r;
    reg [RW+1:0] e14_q;
    reg signed [31:0] e15_n;
    reg signed [31:0] e16_g; reg signed [31:0] e16_n;
    reg signed [63:0] e17_a, e18_a;
    reg signed [EW-1:0] e19_a, e19_s;
    reg signed [31:0] e16_b, e17_b, e18_b;
    reg [OW-1:0] e20_out;
    reg [L-1:0] neg;                    // sign of c: neg[j - 2] belongs to the element in stage e<j>
    reg [63:0] k2, k3, k4, k5, k6, k7, k8, k9, k10, k11, k12, k13, k14, k15;   // {beta, gamma}

    wire [CW-1:0] e2_abs = e2_c[CW-1] ? -e2_c : e2_c;
    wire signed [33:0] root34 = $signed({2'b00, root});
    wire signed [RW+3:0] e15_signed = neg[12] ? -$signed({2'b00, e14_q}) : $signed({2'b00, e14_q});
    wire signed [63:0] e19_sum = e18_a + ($signed(e18_b) <<< 26);    // |n * gamma + beta << 26| < 2^63
    wire signed [EW-1:0] e19_wide = e19_sum;                           // sign extension to EW bits
    wire signed [EW-1:0] e20_in = (XS != 0) ? e19_s : e19_a;
    wire signed [EW-1:0] e20_floor = e20_in >>> SHIFT;
    wire [SH_-1:0] e20_rest = e20_in[SH_-1:0];
    wire signed [EW-1:0] e20_round = (SHIFT == 0) ? e20_in :
        e20_floor + ((e20_rest > HALF || (e20_rest == HALF && e20_floor[0])) ? 64'sd1 : 64'sd0);

    always @(posedge clk) if (ce) begin
        neg <= {neg[L-2:0], 1'b0};
        // e2: center
        e2_c <= $signed({row_q[IW-1], row_q}) - mean; k2 <= coef_q;
        // e3: N = |c| << 26
        e3_n <= {e2_abs[IW-1:0], 26'd0}; neg[1] <= e2_c[CW-1]; k3 <= k2;
        // e4: multiplier inputs; e5..e7: product N * floor(2^P / root) and two more registers
        e4_n <= e3_n; e4_r <= reciprocal; k4 <= k3;
        e5_m <= e4_n * e4_r; e5_n <= e4_n; k5 <= k4;
        e6_m <= e5_m; e6_n <= e5_n; k6 <= k5;
        e7_m <= e6_m; e7_n <= e6_n; k7 <= k6;
        // e8: quotient estimate (at most one below the floor)
        e8_q <= e7_m >> P; e8_n <= e7_n; e8_root <= root; k8 <= k7;
        // e9..e11: estimate * root with registers on both sides
        e9_t <= e8_q * e8_root; e9_q <= e8_q; e9_n <= e8_n; k9 <= k8;
        e10_t <= e9_t; e10_q <= e9_q; e10_n <= e9_n; k10 <= k9;
        e11_t <= e10_t; e11_q <= e10_q; e11_n <= e10_n; k11 <= k10;
        // e12: remainder
        e12_r <= $signed({2'b00, e11_n}) - $signed({1'b0, e11_t[P:0]}); e12_q <= e11_q; k12 <= k11;
        // e13: floor correction
        if (e12_r >= root34) begin e13_q <= e12_q + 1'b1; e13_r <= e12_r - root34; end
        else begin e13_q <= {1'b0, e12_q}; e13_r <= e12_r; end
        k13 <= k12;
        // e14: round half to even
        e14_q <= e13_q + (((e13_r <<< 1) > root34 || ((e13_r <<< 1) == root34 && e13_q[0])) ? 1'b1 : 1'b0);
        k14 <= k13;
        // e15: sign and int32 saturation
        e15_n <= (e15_signed > 64'sd2147483647) ? 32'sd2147483647 :
                 (e15_signed < -64'sd2147483648) ? -32'sd2147483648 : e15_signed[31:0];
        k15 <= k14;
        // e16: multiplier inputs; e17, e18: n * gamma; e19: + beta << 26 at Q56, then << (OQ_s - OQ_LO)
        e16_n <= e15_n; e16_g <= $signed(k15[31:0]); e16_b <= $signed(k15[63:32]);
        e17_a <= e16_n * e16_g; e17_b <= e16_b;
        e18_a <= e17_a; e18_b <= e17_b;
        e19_a <= (XS == 0 && SPREAD > 0) ? (e19_wide <<< pre_set) : e19_wide;      // + the set's pre-shift
        e19_s <= (SPREAD > 0) ? (e19_a <<< pre_set) : e19_a;              // AXIL: the pre-shift in its own stage
        // e20: RNE to the output point and saturation
        e20_out <= (e20_round > OUT_MAX) ? OUT_MAX[OW-1:0] : (e20_round < OUT_MIN) ? OUT_MIN[OW-1:0] : e20_round[OW-1:0];
    end

    always @(posedge clk) begin
        if (!rst_n) ev <= {L{1'b0}};
        else if (ce) ev <= {ev[L-2:0], issue};
    end
    assign m_valid = ev[L-1];
    assign m_data = e20_out;

    // ------------------------------------------------------------------ run-time parameters (AXIL)
    // No frame in progress: no set index held, both row banks empty, no square in flight (a row is full only two
    // cycles after its last code), the statistics idle (the replay has drained: S_DONE follows the row's last
    // accepted output).
    wire frame_idle = !set_held && (full == 2'b00) && !in_v1 && !in_v2 && (state == S_IDLE);
    generate if (AXIL != 0) begin : params
        localparam [AXIL_AW-1:0] SCALARS = 'h1000, COEFS = 'h2000;
        localparam [63:0] EPS_FLOOR = 64'd1 << (2 * ROOT_LOG2);        // floor(sqrt(eps)) >= 2^ROOT_LOG2
        localparam integer WORDS = SETS * C;
        localparam [31:0] ID = 32'h4C4E0001;
        reg hold_r, err_busy, err_range, err_access;
        reg [31:0] writes;
        reg aw_full, w_full, ar_full, rd_wait, bvalid, rvalid;
        reg [AXIL_AW-1:0] aw_addr, ar_addr;
        reg [31:0] w_data, rdata, eps_stage, gamma_stage;
        reg [3:0] w_strb;
        reg [64*SETS-1:0] eps_all = SET_EPS;                            // power-on: the synthesis values
        reg [PW*SETS-1:0] pre_all = pre_init(0);
        reg rd_coef_ok;                                                 // the coefficient read was taken while held

        wire held = hold_r && frame_idle;
        wire [31:0] status = {21'd0, err_access, err_range, err_busy, 6'd0, frame_idle, held};
        // write decode (registered address and data)
        wire w_full_strb = (w_strb == 4'hF);
        wire w_ctrl = aw_addr < SCALARS;
        wire w_scalar = !w_ctrl && (aw_addr < COEFS);
        wire [AXIL_AW-1:0] w_off = aw_addr - COEFS;
        wire w_coef = !w_ctrl && !w_scalar && (w_off[AXIL_AW-1:3] < WORDS);
        wire [7:0] w_set = aw_addr[11:4];
        wire [1:0] w_field = aw_addr[3:2];
        wire w_param = (w_scalar && (w_set < SETS) && (w_field != 2'd1)) || w_coef;
        wire signed [31:0] w_signed = w_data;
        wire oq_ok = (w_signed >= OQ_MIN) && (w_signed <= OQ_MAX);
        wire [63:0] eps_new = {w_data, eps_stage};
        wire eps_ok = (eps_new >= EPS_FLOOR) && (w_data[31:30] == 2'b00);     // floor .. 2^62 - 1
        wire do_write = aw_full && w_full && !bvalid;
        wire coef_we = do_write && w_full_strb && w_coef && held && aw_addr[2];       // the beta write commits
        // read decode
        wire r_ctrl = ar_addr < SCALARS;
        wire r_scalar = !r_ctrl && (ar_addr < COEFS);
        wire [AXIL_AW-1:0] r_off = ar_addr - COEFS;
        wire r_coef = !r_ctrl && !r_scalar && (r_off[AXIL_AW-1:3] < WORDS);
        wire [7:0] r_set = ar_addr[11:4];
        wire [63:0] r_eps = eps_all[64 * r_set +: 64];
        wire [31:0] r_oq = OQ_MIN + pre_all[PW * r_set +: PW];
        wire [CAW-1:0] w_word = w_off[CAW+2:3];
        reg [31:0] read_value;
        always @* begin
            read_value = 32'd0;
            if (r_ctrl) begin
                case (ar_addr[11:2])
                    10'd0: read_value = ID;
                    10'd1: read_value = {31'd0, hold_r};
                    10'd2: read_value = status;
                    10'd3: read_value = writes;
                    10'd4: read_value = {ROWS[15:0], C[15:0]};
                    10'd5: read_value = {ROOT_LOG2[7:0], OQ_MAX[7:0], OQ_MIN[7:0], SETS[7:0] - 8'd1};
                    10'd6: read_value = {8'd0, AXIL_AW[7:0], OW[7:0], IW[7:0]};
                    default: read_value = 32'd0;
                endcase
            end else if (r_scalar) begin
                if (r_set < SETS) case (ar_addr[3:2])
                    2'd0: read_value = r_oq;
                    2'd2: read_value = r_eps[31:0];
                    2'd3: read_value = r_eps[63:32];
                    default: read_value = 32'd0;
                endcase
            end else if (r_coef && rd_coef_ok) read_value = ar_addr[2] ? coef_q[63:32] : coef_q[31:0];
        end

        // coefficient memory write port: one 64-bit write {beta, staged gamma} per channel. The memory stays simple
        // dual-port: AXI reads of gamma and beta use the compute read port, which is free while held.
        always @(posedge clk) if (coef_we) coef[w_word] <= {w_data, gamma_stage};

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
                    else if (w_scalar && ((w_field == 2'd0 && !oq_ok) || (w_field == 2'd3 && !eps_ok))) err_range <= 1'b1;
                    else begin
                        writes <= writes + 1'b1;
                        if (w_scalar && w_field == 2'd0) pre_all[PW * w_set +: PW] <= w_data - OQ_MIN;
                        if (w_scalar && w_field == 2'd2) eps_stage <= w_data;
                        if (w_scalar && w_field == 2'd3) eps_all[64 * w_set +: 64] <= eps_new;
                        if (w_coef && !aw_addr[2]) gamma_stage <= w_data;
                    end
                end
                // a read takes the port when no write does; the data is there one cycle later
                if (ar_full && !rvalid && !rd_wait && !do_write) begin rd_wait <= 1'b1; rd_coef_ok <= held; end
                if (rd_wait) begin
                    rd_wait <= 1'b0; ar_full <= 1'b0; rvalid <= 1'b1; rdata <= read_value;
                end
            end
        end
        assign hold = hold_r;
        assign axil_eps = eps_all[64 * row_set +: 64];
        assign axil_pre = pre_all[PW * row_set +: PW];
        assign axil_coef_read = held;
        assign axil_coef_word = r_off[CAW+2:3];
        assign s_axil_awready = rst_n && !aw_full;
        assign s_axil_wready = rst_n && !w_full;
        assign s_axil_bresp = 2'b00;
        assign s_axil_bvalid = bvalid;
        assign s_axil_arready = rst_n && !ar_full;
        assign s_axil_rdata = rdata;
        assign s_axil_rresp = 2'b00;
        assign s_axil_rvalid = rvalid;
    end else begin : fixed
        assign hold = 1'b0;
        assign axil_eps = 64'd0;
        assign axil_pre = {PW{1'b0}};
        assign axil_coef_read = 1'b0;
        assign axil_coef_word = {CAW{1'b0}};
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
