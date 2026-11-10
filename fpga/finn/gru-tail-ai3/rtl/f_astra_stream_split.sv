// F-ASTRA stream split (data movement): each frame of N0 + N1 beats in, the first N0 beats to m0, the next N1 to m1.
// A beat holds SIMD lanes of IW bits; m0 and m1 lanes hold the low W0 / W1 bits of each lane (W0, W1 <= IW): a part
// whose declared width is narrower than the shared input container (for example 8-bit codes in 12-bit lanes) leaves
// in its own width. The values must fit that width (the frame's contract); nothing is computed.
// One beat per clock, no storage: valid and ready pass through combinationally (FINN FIFOs sit on every side).
// f_astra_stream_unzip (lane mode of the same node): every beat's lanes go to two or three outputs at once (lanes
// [0, G0) to m0, [G0, G0 + G1) to m1, the rest to m2), each lane narrowed to its output's declared width like above.
// A beat leaves when every output has taken it (a broadcast with one taken flag per output, so no output's valid
// waits on another output's ready).
`timescale 1ns/1ps
module f_astra_stream_split #(
    parameter integer SIMD = 4,
    parameter integer IW = 12,          // input lane width
    parameter integer W0 = 12,          // m0 lane width, at most IW
    parameter integer W1 = 12,          // m1 lane width, at most IW
    parameter integer N0 = 9,           // beats per frame to m0
    parameter integer N1 = 16           // beats per frame to m1
)(
    input  wire clk,
    input  wire rst_n,
    input  wire s_valid,
    output wire s_ready,
    input  wire [SIMD*IW-1:0] s_data,
    output wire m0_valid,
    input  wire m0_ready,
    output wire [SIMD*W0-1:0] m0_data,
    output wire m1_valid,
    input  wire m1_ready,
    output wire [SIMD*W1-1:0] m1_data
);
    localparam integer BW = (N0 + N1 > 1) ? $clog2(N0 + N1) : 1;
    reg [BW-1:0] beat;
    wire first = beat < N0;
    assign m0_valid = s_valid && first;
    assign m1_valid = s_valid && !first;
    assign s_ready = first ? m0_ready : m1_ready;
    genvar i;
    generate for (i = 0; i < SIMD; i = i + 1) begin : g_lane
        assign m0_data[i*W0 +: W0] = s_data[i*IW +: W0];
        assign m1_data[i*W1 +: W1] = s_data[i*IW +: W1];
    end endgenerate
    always @(posedge clk) begin
        if (!rst_n) beat <= {BW{1'b0}};
        else if (s_valid && s_ready) beat <= (beat == N0 + N1 - 1) ? {BW{1'b0}} : beat + 1'b1;
    end
endmodule

module f_astra_stream_unzip #(
    parameter integer IW = 32,          // input lane width
    parameter integer G0 = 3,           // lanes per beat to m0
    parameter integer G1 = 3,           // lanes per beat to m1
    parameter integer G2 = 1,           // lanes per beat to m2 (0: two outputs)
    parameter integer W0 = 32,          // m0 lane width, at most IW
    parameter integer W1 = 32,          // m1 lane width, at most IW
    parameter integer W2 = 32           // m2 lane width, at most IW
)(
    input  wire clk,
    input  wire rst_n,
    input  wire s_valid,
    output wire s_ready,
    input  wire [(G0+G1+G2)*IW-1:0] s_data,
    output wire m0_valid,
    input  wire m0_ready,
    output wire [G0*W0-1:0] m0_data,
    output wire m1_valid,
    input  wire m1_ready,
    output wire [G1*W1-1:0] m1_data,
    output wire m2_valid,
    input  wire m2_ready,
    output wire [((G2 > 0) ? G2 : 1)*W2-1:0] m2_data
);
    reg t0, t1, t2;
    wire has2 = G2 > 0;
    assign m0_valid = s_valid && !t0;
    assign m1_valid = s_valid && !t1;
    assign m2_valid = has2 && s_valid && !t2;
    wire d0 = t0 || m0_ready;
    wire d1 = t1 || m1_ready;
    wire d2 = !has2 || t2 || m2_ready;
    assign s_ready = d0 && d1 && d2;
    genvar i;
    generate
        for (i = 0; i < G0; i = i + 1) begin : g_m0
            assign m0_data[i*W0 +: W0] = s_data[i*IW +: W0];
        end
        for (i = 0; i < G1; i = i + 1) begin : g_m1
            assign m1_data[i*W1 +: W1] = s_data[(G0+i)*IW +: W1];
        end
        if (G2 > 0) begin : g_m2
            for (i = 0; i < G2; i = i + 1) begin : g_lane
                assign m2_data[i*W2 +: W2] = s_data[(G0+G1+i)*IW +: W2];
            end
        end else begin : g_none
            assign m2_data = {W2{1'b0}};
        end
    endgenerate
    always @(posedge clk) begin
        if (!rst_n || (s_valid && s_ready)) begin
            t0 <= 1'b0; t1 <= 1'b0; t2 <= 1'b0;
        end else begin
            if (m0_valid && m0_ready) t0 <= 1'b1;
            if (m1_valid && m1_ready) t1 <= 1'b1;
            if (m2_valid && m2_ready) t2 <= 1'b1;
        end
    end
endmodule
