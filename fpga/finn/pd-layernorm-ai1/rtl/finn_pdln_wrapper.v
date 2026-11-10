//Copyright 1986-2020 Xilinx, Inc. All Rights Reserved.
//--------------------------------------------------------------------------------
//Tool Version: Vivado v.2020.2 (win64) Build 3064766 Wed Nov 18 09:12:45 MST 2020
//Date        : Tue Oct 27 19:31:28 2026
//Host        : DESKTOP-SV0I9KV running 64-bit major release  (build 9200)
//Command     : generate_target finn_pdln_wrapper.bd
//Design      : finn_pdln_wrapper
//Purpose     : IP block netlist
//--------------------------------------------------------------------------------
`timescale 1 ps / 1 ps

module finn_pdln_wrapper
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
    s_axis_0_tdata,
    s_axis_0_tready,
    s_axis_0_tvalid,
    s_axis_1_tdata,
    s_axis_1_tready,
    s_axis_1_tvalid,
    sim_finish);
  input ap_clk;
  input ap_rst_n;
  output [15:0]m_axis_0_tdata;
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
  input [15:0]s_axis_0_tdata;
  output s_axis_0_tready;
  input s_axis_0_tvalid;
  input [7:0]s_axis_1_tdata;
  output s_axis_1_tready;
  input s_axis_1_tvalid;
  input sim_finish;

  wire ap_clk;
  wire ap_rst_n;
  wire [15:0]m_axis_0_tdata;
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
  wire [15:0]s_axis_0_tdata;
  wire s_axis_0_tready;
  wire s_axis_0_tvalid;
  wire [7:0]s_axis_1_tdata;
  wire s_axis_1_tready;
  wire s_axis_1_tvalid;
  wire sim_finish;

  finn_pdln finn_pdln_i
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
        .s_axis_0_tdata(s_axis_0_tdata),
        .s_axis_0_tready(s_axis_0_tready),
        .s_axis_0_tvalid(s_axis_0_tvalid),
        .s_axis_1_tdata(s_axis_1_tdata),
        .s_axis_1_tready(s_axis_1_tready),
        .s_axis_1_tvalid(s_axis_1_tvalid),
        .sim_finish(sim_finish));
endmodule
