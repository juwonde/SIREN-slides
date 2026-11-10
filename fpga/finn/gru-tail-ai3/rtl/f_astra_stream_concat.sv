// F-ASTRA stream concat (data movement): each frame takes N0 beats from s0, then N1 beats from s1, and writes them as
// one stream whose SIMD lanes are OW bits wide; a lane of s0 (W0 bits) or s1 (W1 bits) is sign-extended when its
// input is signed (S0, S1) and zero-extended otherwise, so every element keeps its value in a wider container.
// One beat per clock, no storage: valid and ready pass through combinationally (FINN FIFOs sit on every side).
`timescale 1ns/1ps
module f_astra_stream_concat #(
    parameter integer SIMD = 4,
    parameter integer W0 = 12,          // element width of s0
    parameter integer S0 = 1,           // s0 signed
    parameter integer W1 = 8,           // element width of s1
    parameter integer S1 = 0,           // s1 signed
    parameter integer OW = 16,          // output element width, at least W0 and W1
    parameter integer N0 = 16,          // beats per frame from s0
    parameter integer N1 = 7            // beats per frame from s1
)(
    input  wire clk,
    input  wire rst_n,
    input  wire s0_valid,
    output wire s0_ready,
    input  wire [SIMD*W0-1:0] s0_data,
    input  wire s1_valid,
    output wire s1_ready,
    input  wire [SIMD*W1-1:0] s1_data,
    output wire m_valid,
    input  wire m_ready,
    output wire [SIMD*OW-1:0] m_data
);
    localparam integer BW = (N0 + N1 > 1) ? $clog2(N0 + N1) : 1;
    reg [BW-1:0] beat;
    wire first = beat < N0;
    assign m_valid = first ? s0_valid : s1_valid;
    assign s0_ready = first && m_ready;
    assign s1_ready = !first && m_ready;
    genvar i;
    generate for (i = 0; i < SIMD; i = i + 1) begin : g_lane
        wire [W0-1:0] a = s0_data[i*W0 +: W0];
        wire [W1-1:0] b = s1_data[i*W1 +: W1];
        wire [OW-1:0] wa = {{(OW-W0){S0 ? a[W0-1] : 1'b0}}, a};
        wire [OW-1:0] wb = {{(OW-W1){S1 ? b[W1-1] : 1'b0}}, b};
        assign m_data[i*OW +: OW] = first ? wa : wb;
    end endgenerate
    always @(posedge clk) begin
        if (!rst_n) beat <= {BW{1'b0}};
        else if (m_valid && m_ready) beat <= (beat == N0 + N1 - 1) ? {BW{1'b0}} : beat + 1'b1;
    end
endmodule
