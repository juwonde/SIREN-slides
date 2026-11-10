//Copyright 1986-2020 Xilinx, Inc. All Rights Reserved.
//--------------------------------------------------------------------------------
//Tool Version: Vivado v.2020.2 (win64) Build 3064766 Wed Nov 18 09:12:45 MST 2020
//Date        : Thu Nov  5 10:54:48 2026
//Host        : DESKTOP-SV0I9KV running 64-bit major release  (build 9200)
//Command     : generate_target finn_grutail_wrapper.bd
//Design      : finn_grutail_wrapper
//Purpose     : IP block netlist
//--------------------------------------------------------------------------------
`timescale 1 ps / 1 ps

module finn_grutail_wrapper
   (ap_clk,
    ap_rst_n,
    m_axis_0_tdata,
    m_axis_0_tready,
    m_axis_0_tvalid,
    s_axilite_0_araddr,
    s_axilite_0_arready,
    s_axilite_0_arvalid,
    s_axilite_0_awaddr,
    s_axilite_0_awready,
    s_axilite_0_awvalid,
    s_axilite_0_bready,
    s_axilite_0_bresp,
    s_axilite_0_bvalid,
    s_axilite_0_rdata,
    s_axilite_0_rready,
    s_axilite_0_rresp,
    s_axilite_0_rvalid,
    s_axilite_0_wdata,
    s_axilite_0_wready,
    s_axilite_0_wstrb,
    s_axilite_0_wvalid,
    s_axilite_1_araddr,
    s_axilite_1_arprot,
    s_axilite_1_arready,
    s_axilite_1_arvalid,
    s_axilite_1_awaddr,
    s_axilite_1_awprot,
    s_axilite_1_awready,
    s_axilite_1_awvalid,
    s_axilite_1_bready,
    s_axilite_1_bresp,
    s_axilite_1_bvalid,
    s_axilite_1_rdata,
    s_axilite_1_rready,
    s_axilite_1_rresp,
    s_axilite_1_rvalid,
    s_axilite_1_wdata,
    s_axilite_1_wready,
    s_axilite_1_wstrb,
    s_axilite_1_wvalid,
    s_axilite_2_araddr,
    s_axilite_2_arready,
    s_axilite_2_arvalid,
    s_axilite_2_awaddr,
    s_axilite_2_awready,
    s_axilite_2_awvalid,
    s_axilite_2_bready,
    s_axilite_2_bresp,
    s_axilite_2_bvalid,
    s_axilite_2_rdata,
    s_axilite_2_rready,
    s_axilite_2_rresp,
    s_axilite_2_rvalid,
    s_axilite_2_wdata,
    s_axilite_2_wready,
    s_axilite_2_wstrb,
    s_axilite_2_wvalid,
    s_axis_0_tdata,
    s_axis_0_tready,
    s_axis_0_tvalid,
    sim_finish);
  input ap_clk;
  input ap_rst_n;
  output [63:0]m_axis_0_tdata;
  input m_axis_0_tready;
  output m_axis_0_tvalid;
  input [15:0]s_axilite_0_araddr;
  output s_axilite_0_arready;
  input s_axilite_0_arvalid;
  input [15:0]s_axilite_0_awaddr;
  output s_axilite_0_awready;
  input s_axilite_0_awvalid;
  input s_axilite_0_bready;
  output [1:0]s_axilite_0_bresp;
  output s_axilite_0_bvalid;
  output [31:0]s_axilite_0_rdata;
  input s_axilite_0_rready;
  output [1:0]s_axilite_0_rresp;
  output s_axilite_0_rvalid;
  input [31:0]s_axilite_0_wdata;
  output s_axilite_0_wready;
  input [3:0]s_axilite_0_wstrb;
  input s_axilite_0_wvalid;
  input [10:0]s_axilite_1_araddr;
  input [2:0]s_axilite_1_arprot;
  output s_axilite_1_arready;
  input s_axilite_1_arvalid;
  input [10:0]s_axilite_1_awaddr;
  input [2:0]s_axilite_1_awprot;
  output s_axilite_1_awready;
  input s_axilite_1_awvalid;
  input s_axilite_1_bready;
  output [1:0]s_axilite_1_bresp;
  output s_axilite_1_bvalid;
  output [31:0]s_axilite_1_rdata;
  input s_axilite_1_rready;
  output [1:0]s_axilite_1_rresp;
  output s_axilite_1_rvalid;
  input [31:0]s_axilite_1_wdata;
  output s_axilite_1_wready;
  input [3:0]s_axilite_1_wstrb;
  input s_axilite_1_wvalid;
  input [14:0]s_axilite_2_araddr;
  output s_axilite_2_arready;
  input s_axilite_2_arvalid;
  input [14:0]s_axilite_2_awaddr;
  output s_axilite_2_awready;
  input s_axilite_2_awvalid;
  input s_axilite_2_bready;
  output [1:0]s_axilite_2_bresp;
  output s_axilite_2_bvalid;
  output [31:0]s_axilite_2_rdata;
  input s_axilite_2_rready;
  output [1:0]s_axilite_2_rresp;
  output s_axilite_2_rvalid;
  input [31:0]s_axilite_2_wdata;
  output s_axilite_2_wready;
  input [3:0]s_axilite_2_wstrb;
  input s_axilite_2_wvalid;
  input [223:0]s_axis_0_tdata;
  output s_axis_0_tready;
  input s_axis_0_tvalid;
  input sim_finish;

  wire ap_clk;
  wire ap_rst_n;
  wire [63:0]m_axis_0_tdata;
  wire m_axis_0_tready;
  wire m_axis_0_tvalid;
  wire [15:0]s_axilite_0_araddr;
  wire s_axilite_0_arready;
  wire s_axilite_0_arvalid;
  wire [15:0]s_axilite_0_awaddr;
  wire s_axilite_0_awready;
  wire s_axilite_0_awvalid;
  wire s_axilite_0_bready;
  wire [1:0]s_axilite_0_bresp;
  wire s_axilite_0_bvalid;
  wire [31:0]s_axilite_0_rdata;
  wire s_axilite_0_rready;
  wire [1:0]s_axilite_0_rresp;
  wire s_axilite_0_rvalid;
  wire [31:0]s_axilite_0_wdata;
  wire s_axilite_0_wready;
  wire [3:0]s_axilite_0_wstrb;
  wire s_axilite_0_wvalid;
  wire [10:0]s_axilite_1_araddr;
  wire [2:0]s_axilite_1_arprot;
  wire s_axilite_1_arready;
  wire s_axilite_1_arvalid;
  wire [10:0]s_axilite_1_awaddr;
  wire [2:0]s_axilite_1_awprot;
  wire s_axilite_1_awready;
  wire s_axilite_1_awvalid;
  wire s_axilite_1_bready;
  wire [1:0]s_axilite_1_bresp;
  wire s_axilite_1_bvalid;
  wire [31:0]s_axilite_1_rdata;
  wire s_axilite_1_rready;
  wire [1:0]s_axilite_1_rresp;
  wire s_axilite_1_rvalid;
  wire [31:0]s_axilite_1_wdata;
  wire s_axilite_1_wready;
  wire [3:0]s_axilite_1_wstrb;
  wire s_axilite_1_wvalid;
  wire [14:0]s_axilite_2_araddr;
  wire s_axilite_2_arready;
  wire s_axilite_2_arvalid;
  wire [14:0]s_axilite_2_awaddr;
  wire s_axilite_2_awready;
  wire s_axilite_2_awvalid;
  wire s_axilite_2_bready;
  wire [1:0]s_axilite_2_bresp;
  wire s_axilite_2_bvalid;
  wire [31:0]s_axilite_2_rdata;
  wire s_axilite_2_rready;
  wire [1:0]s_axilite_2_rresp;
  wire s_axilite_2_rvalid;
  wire [31:0]s_axilite_2_wdata;
  wire s_axilite_2_wready;
  wire [3:0]s_axilite_2_wstrb;
  wire s_axilite_2_wvalid;
  wire [223:0]s_axis_0_tdata;
  wire s_axis_0_tready;
  wire s_axis_0_tvalid;
  wire sim_finish;

  finn_grutail finn_grutail_i
       (.ap_clk(ap_clk),
        .ap_rst_n(ap_rst_n),
        .m_axis_0_tdata(m_axis_0_tdata),
        .m_axis_0_tready(m_axis_0_tready),
        .m_axis_0_tvalid(m_axis_0_tvalid),
        .s_axilite_0_araddr(s_axilite_0_araddr),
        .s_axilite_0_arready(s_axilite_0_arready),
        .s_axilite_0_arvalid(s_axilite_0_arvalid),
        .s_axilite_0_awaddr(s_axilite_0_awaddr),
        .s_axilite_0_awready(s_axilite_0_awready),
        .s_axilite_0_awvalid(s_axilite_0_awvalid),
        .s_axilite_0_bready(s_axilite_0_bready),
        .s_axilite_0_bresp(s_axilite_0_bresp),
        .s_axilite_0_bvalid(s_axilite_0_bvalid),
        .s_axilite_0_rdata(s_axilite_0_rdata),
        .s_axilite_0_rready(s_axilite_0_rready),
        .s_axilite_0_rresp(s_axilite_0_rresp),
        .s_axilite_0_rvalid(s_axilite_0_rvalid),
        .s_axilite_0_wdata(s_axilite_0_wdata),
        .s_axilite_0_wready(s_axilite_0_wready),
        .s_axilite_0_wstrb(s_axilite_0_wstrb),
        .s_axilite_0_wvalid(s_axilite_0_wvalid),
        .s_axilite_1_araddr(s_axilite_1_araddr),
        .s_axilite_1_arprot(s_axilite_1_arprot),
        .s_axilite_1_arready(s_axilite_1_arready),
        .s_axilite_1_arvalid(s_axilite_1_arvalid),
        .s_axilite_1_awaddr(s_axilite_1_awaddr),
        .s_axilite_1_awprot(s_axilite_1_awprot),
        .s_axilite_1_awready(s_axilite_1_awready),
        .s_axilite_1_awvalid(s_axilite_1_awvalid),
        .s_axilite_1_bready(s_axilite_1_bready),
        .s_axilite_1_bresp(s_axilite_1_bresp),
        .s_axilite_1_bvalid(s_axilite_1_bvalid),
        .s_axilite_1_rdata(s_axilite_1_rdata),
        .s_axilite_1_rready(s_axilite_1_rready),
        .s_axilite_1_rresp(s_axilite_1_rresp),
        .s_axilite_1_rvalid(s_axilite_1_rvalid),
        .s_axilite_1_wdata(s_axilite_1_wdata),
        .s_axilite_1_wready(s_axilite_1_wready),
        .s_axilite_1_wstrb(s_axilite_1_wstrb),
        .s_axilite_1_wvalid(s_axilite_1_wvalid),
        .s_axilite_2_araddr(s_axilite_2_araddr),
        .s_axilite_2_arready(s_axilite_2_arready),
        .s_axilite_2_arvalid(s_axilite_2_arvalid),
        .s_axilite_2_awaddr(s_axilite_2_awaddr),
        .s_axilite_2_awready(s_axilite_2_awready),
        .s_axilite_2_awvalid(s_axilite_2_awvalid),
        .s_axilite_2_bready(s_axilite_2_bready),
        .s_axilite_2_bresp(s_axilite_2_bresp),
        .s_axilite_2_bvalid(s_axilite_2_bvalid),
        .s_axilite_2_rdata(s_axilite_2_rdata),
        .s_axilite_2_rready(s_axilite_2_rready),
        .s_axilite_2_rresp(s_axilite_2_rresp),
        .s_axilite_2_rvalid(s_axilite_2_rvalid),
        .s_axilite_2_wdata(s_axilite_2_wdata),
        .s_axilite_2_wready(s_axilite_2_wready),
        .s_axilite_2_wstrb(s_axilite_2_wstrb),
        .s_axilite_2_wvalid(s_axilite_2_wvalid),
        .s_axis_0_tdata(s_axis_0_tdata),
        .s_axis_0_tready(s_axis_0_tready),
        .s_axis_0_tvalid(s_axis_0_tvalid),
        .sim_finish(sim_finish));
endmodule
