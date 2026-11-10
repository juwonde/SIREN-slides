//Copyright 1986-2020 Xilinx, Inc. All Rights Reserved.
//--------------------------------------------------------------------------------
//Tool Version: Vivado v.2020.2 (win64) Build 3064766 Wed Nov 18 09:12:45 MST 2020
//Date        : Thu Nov  5 10:54:48 2026
//Host        : DESKTOP-SV0I9KV running 64-bit major release  (build 9200)
//Command     : generate_target finn_grutail.bd
//Design      : finn_grutail
//Purpose     : IP block netlist
//--------------------------------------------------------------------------------
`timescale 1 ps / 1 ps

module GTMVH0_imp_YL0CMH
   (ap_clk,
    ap_rst_n,
    in0_V_tdata,
    in0_V_tready,
    in0_V_tvalid,
    out0_V_tdata,
    out0_V_tready,
    out0_V_tvalid,
    s_axilite_araddr,
    s_axilite_arprot,
    s_axilite_arready,
    s_axilite_arvalid,
    s_axilite_awaddr,
    s_axilite_awprot,
    s_axilite_awready,
    s_axilite_awvalid,
    s_axilite_bready,
    s_axilite_bresp,
    s_axilite_bvalid,
    s_axilite_rdata,
    s_axilite_rready,
    s_axilite_rresp,
    s_axilite_rvalid,
    s_axilite_wdata,
    s_axilite_wready,
    s_axilite_wstrb,
    s_axilite_wvalid);
  input ap_clk;
  input ap_rst_n;
  input [47:0]in0_V_tdata;
  output in0_V_tready;
  input in0_V_tvalid;
  output [183:0]out0_V_tdata;
  input out0_V_tready;
  output out0_V_tvalid;
  input [10:0]s_axilite_araddr;
  input [2:0]s_axilite_arprot;
  output s_axilite_arready;
  input s_axilite_arvalid;
  input [10:0]s_axilite_awaddr;
  input [2:0]s_axilite_awprot;
  output s_axilite_awready;
  input s_axilite_awvalid;
  input s_axilite_bready;
  output [1:0]s_axilite_bresp;
  output s_axilite_bvalid;
  output [31:0]s_axilite_rdata;
  input s_axilite_rready;
  output [1:0]s_axilite_rresp;
  output s_axilite_rvalid;
  input [31:0]s_axilite_wdata;
  output s_axilite_wready;
  input [3:0]s_axilite_wstrb;
  input s_axilite_wvalid;

  wire [183:0]GTMVH0_out0_V_V_TDATA;
  wire GTMVH0_out0_V_V_TREADY;
  wire GTMVH0_out0_V_V_TVALID;
  wire [223:0]GTMVH0_wstrm_m_axis_0_TDATA;
  wire GTMVH0_wstrm_m_axis_0_TREADY;
  wire GTMVH0_wstrm_m_axis_0_TVALID;
  wire ap_clk_1;
  wire ap_rst_n_1;
  wire [47:0]in0_V_1_TDATA;
  wire in0_V_1_TREADY;
  wire in0_V_1_TVALID;
  wire [10:0]s_axilite_1_ARADDR;
  wire [2:0]s_axilite_1_ARPROT;
  wire s_axilite_1_ARREADY;
  wire s_axilite_1_ARVALID;
  wire [10:0]s_axilite_1_AWADDR;
  wire [2:0]s_axilite_1_AWPROT;
  wire s_axilite_1_AWREADY;
  wire s_axilite_1_AWVALID;
  wire s_axilite_1_BREADY;
  wire [1:0]s_axilite_1_BRESP;
  wire s_axilite_1_BVALID;
  wire [31:0]s_axilite_1_RDATA;
  wire s_axilite_1_RREADY;
  wire [1:0]s_axilite_1_RRESP;
  wire s_axilite_1_RVALID;
  wire [31:0]s_axilite_1_WDATA;
  wire s_axilite_1_WREADY;
  wire [3:0]s_axilite_1_WSTRB;
  wire s_axilite_1_WVALID;

  assign GTMVH0_out0_V_V_TREADY = out0_V_tready;
  assign ap_clk_1 = ap_clk;
  assign ap_rst_n_1 = ap_rst_n;
  assign in0_V_1_TDATA = in0_V_tdata[47:0];
  assign in0_V_1_TVALID = in0_V_tvalid;
  assign in0_V_tready = in0_V_1_TREADY;
  assign out0_V_tdata[183:0] = GTMVH0_out0_V_V_TDATA;
  assign out0_V_tvalid = GTMVH0_out0_V_V_TVALID;
  assign s_axilite_1_ARADDR = s_axilite_araddr[10:0];
  assign s_axilite_1_ARPROT = s_axilite_arprot[2:0];
  assign s_axilite_1_ARVALID = s_axilite_arvalid;
  assign s_axilite_1_AWADDR = s_axilite_awaddr[10:0];
  assign s_axilite_1_AWPROT = s_axilite_awprot[2:0];
  assign s_axilite_1_AWVALID = s_axilite_awvalid;
  assign s_axilite_1_BREADY = s_axilite_bready;
  assign s_axilite_1_RREADY = s_axilite_rready;
  assign s_axilite_1_WDATA = s_axilite_wdata[31:0];
  assign s_axilite_1_WSTRB = s_axilite_wstrb[3:0];
  assign s_axilite_1_WVALID = s_axilite_wvalid;
  assign s_axilite_arready = s_axilite_1_ARREADY;
  assign s_axilite_awready = s_axilite_1_AWREADY;
  assign s_axilite_bresp[1:0] = s_axilite_1_BRESP;
  assign s_axilite_bvalid = s_axilite_1_BVALID;
  assign s_axilite_rdata[31:0] = s_axilite_1_RDATA;
  assign s_axilite_rresp[1:0] = s_axilite_1_RRESP;
  assign s_axilite_rvalid = s_axilite_1_RVALID;
  assign s_axilite_wready = s_axilite_1_WREADY;
  finn_grutail_GTMVH0_0 GTMVH0
       (.ap_clk(ap_clk_1),
        .ap_rst_n(ap_rst_n_1),
        .in0_V_V_TDATA(in0_V_1_TDATA),
        .in0_V_V_TREADY(in0_V_1_TREADY),
        .in0_V_V_TVALID(in0_V_1_TVALID),
        .in1_V_V_TDATA(GTMVH0_wstrm_m_axis_0_TDATA),
        .in1_V_V_TREADY(GTMVH0_wstrm_m_axis_0_TREADY),
        .in1_V_V_TVALID(GTMVH0_wstrm_m_axis_0_TVALID),
        .out0_V_V_TDATA(GTMVH0_out0_V_V_TDATA),
        .out0_V_V_TREADY(GTMVH0_out0_V_V_TREADY),
        .out0_V_V_TVALID(GTMVH0_out0_V_V_TVALID));
  finn_grutail_GTMVH0_wstrm_0 GTMVH0_wstrm
       (.ap_clk(ap_clk_1),
        .ap_clk2x(ap_clk_1),
        .ap_rst_n(ap_rst_n_1),
        .m_axis_0_tdata(GTMVH0_wstrm_m_axis_0_TDATA),
        .m_axis_0_tready(GTMVH0_wstrm_m_axis_0_TREADY),
        .m_axis_0_tvalid(GTMVH0_wstrm_m_axis_0_TVALID),
        .s_axilite_ARADDR(s_axilite_1_ARADDR),
        .s_axilite_ARPROT(s_axilite_1_ARPROT),
        .s_axilite_ARREADY(s_axilite_1_ARREADY),
        .s_axilite_ARVALID(s_axilite_1_ARVALID),
        .s_axilite_AWADDR(s_axilite_1_AWADDR),
        .s_axilite_AWPROT(s_axilite_1_AWPROT),
        .s_axilite_AWREADY(s_axilite_1_AWREADY),
        .s_axilite_AWVALID(s_axilite_1_AWVALID),
        .s_axilite_BREADY(s_axilite_1_BREADY),
        .s_axilite_BRESP(s_axilite_1_BRESP),
        .s_axilite_BVALID(s_axilite_1_BVALID),
        .s_axilite_RDATA(s_axilite_1_RDATA),
        .s_axilite_RREADY(s_axilite_1_RREADY),
        .s_axilite_RRESP(s_axilite_1_RRESP),
        .s_axilite_RVALID(s_axilite_1_RVALID),
        .s_axilite_WDATA(s_axilite_1_WDATA),
        .s_axilite_WREADY(s_axilite_1_WREADY),
        .s_axilite_WSTRB(s_axilite_1_WSTRB),
        .s_axilite_WVALID(s_axilite_1_WVALID),
        .s_axis_0_tdata(1'b0),
        .s_axis_0_tvalid(1'b0));
endmodule

(* CORE_GENERATION_INFO = "finn_grutail,IP_Integrator,{x_ipVendor=xilinx.com,x_ipLibrary=BlockDiagram,x_ipName=finn_grutail,x_ipVersion=1.00.a,x_ipLanguage=VERILOG,numBlks=25,numReposBlks=24,numNonXlnxBlks=0,numHierBlks=1,maxHierDepth=1,numSysgenBlks=0,numHlsBlks=2,numHdlrefBlks=22,numPkgbdBlks=0,bdsource=USER,synth_mode=OOC_per_IP}" *) (* HW_HANDOFF = "finn_grutail.hwdef" *) 
module finn_grutail
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
  (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 CLK.AP_CLK CLK" *) (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME CLK.AP_CLK, ASSOCIATED_BUSIF s_axilite_0:s_axilite_1:s_axilite_2:s_axis_0:m_axis_0, ASSOCIATED_RESET ap_rst_n, CLK_DOMAIN finn_grutail_ap_clk_0, FREQ_HZ 125000000, FREQ_TOLERANCE_HZ 0, INSERT_VIP 0, PHASE 0.000" *) input ap_clk;
  (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 RST.AP_RST_N RST" *) (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME RST.AP_RST_N, INSERT_VIP 0, POLARITY ACTIVE_LOW" *) input ap_rst_n;
  (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 m_axis_0 " *) (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME m_axis_0, CLK_DOMAIN finn_grutail_ap_clk_0, FREQ_HZ 125000000, HAS_TKEEP 0, HAS_TLAST 0, HAS_TREADY 1, HAS_TSTRB 0, INSERT_VIP 0, LAYERED_METADATA undef, PHASE 0.000, TDATA_NUM_BYTES 8, TDEST_WIDTH 0, TID_WIDTH 0, TUSER_WIDTH 0" *) output [63:0]m_axis_0_tdata;
  (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 m_axis_0 " *) input m_axis_0_tready;
  (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 m_axis_0 " *) output m_axis_0_tvalid;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 ARADDR" *) (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME s_axilite_0, ADDR_WIDTH 16, ARUSER_WIDTH 0, AWUSER_WIDTH 0, BUSER_WIDTH 0, CLK_DOMAIN finn_grutail_ap_clk_0, DATA_WIDTH 32, FREQ_HZ 125000000, HAS_BRESP 1, HAS_BURST 0, HAS_CACHE 0, HAS_LOCK 0, HAS_PROT 0, HAS_QOS 0, HAS_REGION 0, HAS_RRESP 1, HAS_WSTRB 1, ID_WIDTH 0, INSERT_VIP 0, MAX_BURST_LENGTH 1, NUM_READ_OUTSTANDING 1, NUM_READ_THREADS 1, NUM_WRITE_OUTSTANDING 1, NUM_WRITE_THREADS 1, PHASE 0.000, PROTOCOL AXI4LITE, READ_WRITE_MODE READ_WRITE, RUSER_BITS_PER_BYTE 0, RUSER_WIDTH 0, SUPPORTS_NARROW_BURST 0, WUSER_BITS_PER_BYTE 0, WUSER_WIDTH 0" *) input [15:0]s_axilite_0_araddr;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 ARREADY" *) output s_axilite_0_arready;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 ARVALID" *) input s_axilite_0_arvalid;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 AWADDR" *) input [15:0]s_axilite_0_awaddr;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 AWREADY" *) output s_axilite_0_awready;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 AWVALID" *) input s_axilite_0_awvalid;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 BREADY" *) input s_axilite_0_bready;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 BRESP" *) output [1:0]s_axilite_0_bresp;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 BVALID" *) output s_axilite_0_bvalid;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 RDATA" *) output [31:0]s_axilite_0_rdata;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 RREADY" *) input s_axilite_0_rready;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 RRESP" *) output [1:0]s_axilite_0_rresp;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 RVALID" *) output s_axilite_0_rvalid;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 WDATA" *) input [31:0]s_axilite_0_wdata;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 WREADY" *) output s_axilite_0_wready;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 WSTRB" *) input [3:0]s_axilite_0_wstrb;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 WVALID" *) input s_axilite_0_wvalid;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_1 ARADDR" *) (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME s_axilite_1, ADDR_WIDTH 11, ARUSER_WIDTH 0, AWUSER_WIDTH 0, BUSER_WIDTH 0, CLK_DOMAIN finn_grutail_ap_clk_0, DATA_WIDTH 32, FREQ_HZ 125000000, HAS_BRESP 1, HAS_BURST 0, HAS_CACHE 0, HAS_LOCK 0, HAS_PROT 1, HAS_QOS 0, HAS_REGION 0, HAS_RRESP 1, HAS_WSTRB 1, ID_WIDTH 0, INSERT_VIP 0, MAX_BURST_LENGTH 1, NUM_READ_OUTSTANDING 1, NUM_READ_THREADS 1, NUM_WRITE_OUTSTANDING 1, NUM_WRITE_THREADS 1, PHASE 0.000, PROTOCOL AXI4LITE, READ_WRITE_MODE READ_WRITE, RUSER_BITS_PER_BYTE 0, RUSER_WIDTH 0, SUPPORTS_NARROW_BURST 0, WUSER_BITS_PER_BYTE 0, WUSER_WIDTH 0" *) input [10:0]s_axilite_1_araddr;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_1 ARPROT" *) input [2:0]s_axilite_1_arprot;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_1 ARREADY" *) output s_axilite_1_arready;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_1 ARVALID" *) input s_axilite_1_arvalid;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_1 AWADDR" *) input [10:0]s_axilite_1_awaddr;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_1 AWPROT" *) input [2:0]s_axilite_1_awprot;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_1 AWREADY" *) output s_axilite_1_awready;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_1 AWVALID" *) input s_axilite_1_awvalid;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_1 BREADY" *) input s_axilite_1_bready;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_1 BRESP" *) output [1:0]s_axilite_1_bresp;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_1 BVALID" *) output s_axilite_1_bvalid;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_1 RDATA" *) output [31:0]s_axilite_1_rdata;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_1 RREADY" *) input s_axilite_1_rready;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_1 RRESP" *) output [1:0]s_axilite_1_rresp;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_1 RVALID" *) output s_axilite_1_rvalid;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_1 WDATA" *) input [31:0]s_axilite_1_wdata;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_1 WREADY" *) output s_axilite_1_wready;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_1 WSTRB" *) input [3:0]s_axilite_1_wstrb;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_1 WVALID" *) input s_axilite_1_wvalid;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_2 ARADDR" *) (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME s_axilite_2, ADDR_WIDTH 15, ARUSER_WIDTH 0, AWUSER_WIDTH 0, BUSER_WIDTH 0, CLK_DOMAIN finn_grutail_ap_clk_0, DATA_WIDTH 32, FREQ_HZ 125000000, HAS_BRESP 1, HAS_BURST 0, HAS_CACHE 0, HAS_LOCK 0, HAS_PROT 0, HAS_QOS 0, HAS_REGION 0, HAS_RRESP 1, HAS_WSTRB 1, ID_WIDTH 0, INSERT_VIP 0, MAX_BURST_LENGTH 1, NUM_READ_OUTSTANDING 1, NUM_READ_THREADS 1, NUM_WRITE_OUTSTANDING 1, NUM_WRITE_THREADS 1, PHASE 0.000, PROTOCOL AXI4LITE, READ_WRITE_MODE READ_WRITE, RUSER_BITS_PER_BYTE 0, RUSER_WIDTH 0, SUPPORTS_NARROW_BURST 0, WUSER_BITS_PER_BYTE 0, WUSER_WIDTH 0" *) input [14:0]s_axilite_2_araddr;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_2 ARREADY" *) output s_axilite_2_arready;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_2 ARVALID" *) input s_axilite_2_arvalid;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_2 AWADDR" *) input [14:0]s_axilite_2_awaddr;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_2 AWREADY" *) output s_axilite_2_awready;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_2 AWVALID" *) input s_axilite_2_awvalid;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_2 BREADY" *) input s_axilite_2_bready;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_2 BRESP" *) output [1:0]s_axilite_2_bresp;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_2 BVALID" *) output s_axilite_2_bvalid;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_2 RDATA" *) output [31:0]s_axilite_2_rdata;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_2 RREADY" *) input s_axilite_2_rready;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_2 RRESP" *) output [1:0]s_axilite_2_rresp;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_2 RVALID" *) output s_axilite_2_rvalid;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_2 WDATA" *) input [31:0]s_axilite_2_wdata;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_2 WREADY" *) output s_axilite_2_wready;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_2 WSTRB" *) input [3:0]s_axilite_2_wstrb;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_2 WVALID" *) input s_axilite_2_wvalid;
  (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 s_axis_0 " *) (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME s_axis_0, CLK_DOMAIN finn_grutail_ap_clk_0, FREQ_HZ 125000000, HAS_TKEEP 0, HAS_TLAST 0, HAS_TREADY 1, HAS_TSTRB 0, INSERT_VIP 0, LAYERED_METADATA undef, PHASE 0.000, TDATA_NUM_BYTES 28, TDEST_WIDTH 0, TID_WIDTH 0, TUSER_WIDTH 0" *) input [223:0]s_axis_0_tdata;
  (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 s_axis_0 " *) output s_axis_0_tready;
  (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 s_axis_0 " *) input s_axis_0_tvalid;
  input sim_finish;

  wire [63:0]GTDPH0_out0_V_V_TDATA;
  wire GTDPH0_out0_V_V_TREADY;
  wire GTDPH0_out0_V_V_TVALID;
  wire [63:0]GTDPH0_out1_V_V_TDATA;
  wire GTDPH0_out1_V_V_TREADY;
  wire GTDPH0_out1_V_V_TVALID;
  wire [47:0]GTDWR0_out0_V_TDATA;
  wire GTDWR0_out0_V_TREADY;
  wire GTDWR0_out0_V_TVALID;
  wire [31:0]GTDWR1_out0_V_TDATA;
  wire GTDWR1_out0_V_TREADY;
  wire GTDWR1_out0_V_TVALID;
  wire [31:0]GTDWR2_out0_V_TDATA;
  wire GTDWR2_out0_V_TREADY;
  wire GTDWR2_out0_V_TVALID;
  wire [223:0]GTFFR0_out0_V_TDATA;
  wire GTFFR0_out0_V_TREADY;
  wire GTFFR0_out0_V_TVALID;
  wire [7:0]GTFFR10_out0_V_TDATA;
  wire GTFFR10_out0_V_TREADY;
  wire GTFFR10_out0_V_TVALID;
  wire [31:0]GTFFR11_out0_V_TDATA;
  wire GTFFR11_out0_V_TREADY;
  wire GTFFR11_out0_V_TVALID;
  wire [63:0]GTFFR12_out0_V_TDATA;
  wire GTFFR12_out0_V_TREADY;
  wire GTFFR12_out0_V_TVALID;
  wire [79:0]GTFFR1_out0_V_TDATA;
  wire GTFFR1_out0_V_TREADY;
  wire GTFFR1_out0_V_TVALID;
  wire [79:0]GTFFR2_out0_V_TDATA;
  wire GTFFR2_out0_V_TREADY;
  wire GTFFR2_out0_V_TVALID;
  wire [15:0]GTFFR3_out0_V_TDATA;
  wire GTFFR3_out0_V_TREADY;
  wire GTFFR3_out0_V_TVALID;
  wire [15:0]GTFFR4_out0_V_TDATA;
  wire GTFFR4_out0_V_TREADY;
  wire GTFFR4_out0_V_TVALID;
  wire [47:0]GTFFR5_out0_V_TDATA;
  wire GTFFR5_out0_V_TREADY;
  wire GTFFR5_out0_V_TVALID;
  wire [47:0]GTFFR6_out0_V_TDATA;
  wire GTFFR6_out0_V_TREADY;
  wire GTFFR6_out0_V_TVALID;
  wire [47:0]GTFFR7_out0_V_TDATA;
  wire GTFFR7_out0_V_TREADY;
  wire GTFFR7_out0_V_TVALID;
  wire [183:0]GTFFR8_out0_V_TDATA;
  wire GTFFR8_out0_V_TREADY;
  wire GTFFR8_out0_V_TVALID;
  wire [31:0]GTFFR9_out0_V_TDATA;
  wire GTFFR9_out0_V_TREADY;
  wire GTFFR9_out0_V_TVALID;
  wire [15:0]GTIGR0_out0_V_TDATA;
  wire GTIGR0_out0_V_TREADY;
  wire GTIGR0_out0_V_TVALID;
  wire [79:0]GTISR0_out0_V_TDATA;
  wire GTISR0_out0_V_TREADY;
  wire GTISR0_out0_V_TVALID;
  wire [79:0]GTISR0_out1_V_TDATA;
  wire GTISR0_out1_V_TREADY;
  wire GTISR0_out1_V_TVALID;
  wire [15:0]GTISR0_out2_V_TDATA;
  wire GTISR0_out2_V_TREADY;
  wire GTISR0_out2_V_TVALID;
  wire [63:0]GTISR1_out0_V_TDATA;
  wire GTISR1_out0_V_TREADY;
  wire GTISR1_out0_V_TVALID;
  wire [183:0]GTMVH0_out0_V_TDATA;
  wire GTMVH0_out0_V_TREADY;
  wire GTMVH0_out0_V_TVALID;
  wire [7:0]GTTHR0_out0_V_TDATA;
  wire GTTHR0_out0_V_TREADY;
  wire GTTHR0_out0_V_TVALID;
  wire ap_clk_0_1;
  wire ap_rst_n_0_1;
  wire [223:0]in0_V_0_1_TDATA;
  wire in0_V_0_1_TREADY;
  wire in0_V_0_1_TVALID;
  wire [15:0]s_axilite_0_1_ARADDR;
  wire s_axilite_0_1_ARREADY;
  wire s_axilite_0_1_ARVALID;
  wire [15:0]s_axilite_0_1_AWADDR;
  wire s_axilite_0_1_AWREADY;
  wire s_axilite_0_1_AWVALID;
  wire s_axilite_0_1_BREADY;
  wire [1:0]s_axilite_0_1_BRESP;
  wire s_axilite_0_1_BVALID;
  wire [31:0]s_axilite_0_1_RDATA;
  wire s_axilite_0_1_RREADY;
  wire [1:0]s_axilite_0_1_RRESP;
  wire s_axilite_0_1_RVALID;
  wire [31:0]s_axilite_0_1_WDATA;
  wire s_axilite_0_1_WREADY;
  wire [3:0]s_axilite_0_1_WSTRB;
  wire s_axilite_0_1_WVALID;
  wire [10:0]s_axilite_1_1_ARADDR;
  wire [2:0]s_axilite_1_1_ARPROT;
  wire s_axilite_1_1_ARREADY;
  wire s_axilite_1_1_ARVALID;
  wire [10:0]s_axilite_1_1_AWADDR;
  wire [2:0]s_axilite_1_1_AWPROT;
  wire s_axilite_1_1_AWREADY;
  wire s_axilite_1_1_AWVALID;
  wire s_axilite_1_1_BREADY;
  wire [1:0]s_axilite_1_1_BRESP;
  wire s_axilite_1_1_BVALID;
  wire [31:0]s_axilite_1_1_RDATA;
  wire s_axilite_1_1_RREADY;
  wire [1:0]s_axilite_1_1_RRESP;
  wire s_axilite_1_1_RVALID;
  wire [31:0]s_axilite_1_1_WDATA;
  wire s_axilite_1_1_WREADY;
  wire [3:0]s_axilite_1_1_WSTRB;
  wire s_axilite_1_1_WVALID;
  wire [14:0]s_axilite_2_1_ARADDR;
  wire s_axilite_2_1_ARREADY;
  wire s_axilite_2_1_ARVALID;
  wire [14:0]s_axilite_2_1_AWADDR;
  wire s_axilite_2_1_AWREADY;
  wire s_axilite_2_1_AWVALID;
  wire s_axilite_2_1_BREADY;
  wire [1:0]s_axilite_2_1_BRESP;
  wire s_axilite_2_1_BVALID;
  wire [31:0]s_axilite_2_1_RDATA;
  wire s_axilite_2_1_RREADY;
  wire [1:0]s_axilite_2_1_RRESP;
  wire s_axilite_2_1_RVALID;
  wire [31:0]s_axilite_2_1_WDATA;
  wire s_axilite_2_1_WREADY;
  wire [3:0]s_axilite_2_1_WSTRB;
  wire s_axilite_2_1_WVALID;
  wire sim_finish_0_1;

  assign GTFFR12_out0_V_TREADY = m_axis_0_tready;
  assign ap_clk_0_1 = ap_clk;
  assign ap_rst_n_0_1 = ap_rst_n;
  assign in0_V_0_1_TDATA = s_axis_0_tdata[223:0];
  assign in0_V_0_1_TVALID = s_axis_0_tvalid;
  assign m_axis_0_tdata[63:0] = GTFFR12_out0_V_TDATA;
  assign m_axis_0_tvalid = GTFFR12_out0_V_TVALID;
  assign s_axilite_0_1_ARADDR = s_axilite_0_araddr[15:0];
  assign s_axilite_0_1_ARVALID = s_axilite_0_arvalid;
  assign s_axilite_0_1_AWADDR = s_axilite_0_awaddr[15:0];
  assign s_axilite_0_1_AWVALID = s_axilite_0_awvalid;
  assign s_axilite_0_1_BREADY = s_axilite_0_bready;
  assign s_axilite_0_1_RREADY = s_axilite_0_rready;
  assign s_axilite_0_1_WDATA = s_axilite_0_wdata[31:0];
  assign s_axilite_0_1_WSTRB = s_axilite_0_wstrb[3:0];
  assign s_axilite_0_1_WVALID = s_axilite_0_wvalid;
  assign s_axilite_0_arready = s_axilite_0_1_ARREADY;
  assign s_axilite_0_awready = s_axilite_0_1_AWREADY;
  assign s_axilite_0_bresp[1:0] = s_axilite_0_1_BRESP;
  assign s_axilite_0_bvalid = s_axilite_0_1_BVALID;
  assign s_axilite_0_rdata[31:0] = s_axilite_0_1_RDATA;
  assign s_axilite_0_rresp[1:0] = s_axilite_0_1_RRESP;
  assign s_axilite_0_rvalid = s_axilite_0_1_RVALID;
  assign s_axilite_0_wready = s_axilite_0_1_WREADY;
  assign s_axilite_1_1_ARADDR = s_axilite_1_araddr[10:0];
  assign s_axilite_1_1_ARPROT = s_axilite_1_arprot[2:0];
  assign s_axilite_1_1_ARVALID = s_axilite_1_arvalid;
  assign s_axilite_1_1_AWADDR = s_axilite_1_awaddr[10:0];
  assign s_axilite_1_1_AWPROT = s_axilite_1_awprot[2:0];
  assign s_axilite_1_1_AWVALID = s_axilite_1_awvalid;
  assign s_axilite_1_1_BREADY = s_axilite_1_bready;
  assign s_axilite_1_1_RREADY = s_axilite_1_rready;
  assign s_axilite_1_1_WDATA = s_axilite_1_wdata[31:0];
  assign s_axilite_1_1_WSTRB = s_axilite_1_wstrb[3:0];
  assign s_axilite_1_1_WVALID = s_axilite_1_wvalid;
  assign s_axilite_1_arready = s_axilite_1_1_ARREADY;
  assign s_axilite_1_awready = s_axilite_1_1_AWREADY;
  assign s_axilite_1_bresp[1:0] = s_axilite_1_1_BRESP;
  assign s_axilite_1_bvalid = s_axilite_1_1_BVALID;
  assign s_axilite_1_rdata[31:0] = s_axilite_1_1_RDATA;
  assign s_axilite_1_rresp[1:0] = s_axilite_1_1_RRESP;
  assign s_axilite_1_rvalid = s_axilite_1_1_RVALID;
  assign s_axilite_1_wready = s_axilite_1_1_WREADY;
  assign s_axilite_2_1_ARADDR = s_axilite_2_araddr[14:0];
  assign s_axilite_2_1_ARVALID = s_axilite_2_arvalid;
  assign s_axilite_2_1_AWADDR = s_axilite_2_awaddr[14:0];
  assign s_axilite_2_1_AWVALID = s_axilite_2_awvalid;
  assign s_axilite_2_1_BREADY = s_axilite_2_bready;
  assign s_axilite_2_1_RREADY = s_axilite_2_rready;
  assign s_axilite_2_1_WDATA = s_axilite_2_wdata[31:0];
  assign s_axilite_2_1_WSTRB = s_axilite_2_wstrb[3:0];
  assign s_axilite_2_1_WVALID = s_axilite_2_wvalid;
  assign s_axilite_2_arready = s_axilite_2_1_ARREADY;
  assign s_axilite_2_awready = s_axilite_2_1_AWREADY;
  assign s_axilite_2_bresp[1:0] = s_axilite_2_1_BRESP;
  assign s_axilite_2_bvalid = s_axilite_2_1_BVALID;
  assign s_axilite_2_rdata[31:0] = s_axilite_2_1_RDATA;
  assign s_axilite_2_rresp[1:0] = s_axilite_2_1_RRESP;
  assign s_axilite_2_rvalid = s_axilite_2_1_RVALID;
  assign s_axilite_2_wready = s_axilite_2_1_WREADY;
  assign s_axis_0_tready = in0_V_0_1_TREADY;
  assign sim_finish_0_1 = sim_finish;
  finn_grutail_GTDPH0_0 GTDPH0
       (.ap_clk(ap_clk_0_1),
        .ap_rst_n(ap_rst_n_0_1),
        .in0_V_V_TDATA({1'b0,1'b0,1'b0,1'b0,1'b0,1'b0,1'b0,1'b0,1'b0,1'b0,1'b0,1'b0,1'b0,1'b0,1'b0,1'b0,GTFFR5_out0_V_TDATA}),
        .in0_V_V_TREADY(GTFFR5_out0_V_TREADY),
        .in0_V_V_TVALID(GTFFR5_out0_V_TVALID),
        .out0_V_V_TDATA(GTDPH0_out0_V_V_TDATA),
        .out0_V_V_TREADY(GTDPH0_out0_V_V_TREADY),
        .out0_V_V_TVALID(GTDPH0_out0_V_V_TVALID),
        .out1_V_V_TDATA(GTDPH0_out1_V_V_TDATA),
        .out1_V_V_TREADY(GTDPH0_out1_V_V_TREADY),
        .out1_V_V_TVALID(GTDPH0_out1_V_V_TVALID));
  finn_grutail_GTDWR0_0 GTDWR0
       (.ap_clk(ap_clk_0_1),
        .ap_rst_n(ap_rst_n_0_1),
        .in0_V_TDATA(GTFFR4_out0_V_TDATA),
        .in0_V_TREADY(GTFFR4_out0_V_TREADY),
        .in0_V_TVALID(GTFFR4_out0_V_TVALID),
        .out0_V_TDATA(GTDWR0_out0_V_TDATA),
        .out0_V_TREADY(GTDWR0_out0_V_TREADY),
        .out0_V_TVALID(GTDWR0_out0_V_TVALID));
  finn_grutail_GTDWR1_0 GTDWR1
       (.ap_clk(ap_clk_0_1),
        .ap_rst_n(ap_rst_n_0_1),
        .in0_V_TDATA(GTFFR8_out0_V_TDATA),
        .in0_V_TREADY(GTFFR8_out0_V_TREADY),
        .in0_V_TVALID(GTFFR8_out0_V_TVALID),
        .out0_V_TDATA(GTDWR1_out0_V_TDATA),
        .out0_V_TREADY(GTDWR1_out0_V_TREADY),
        .out0_V_TVALID(GTDWR1_out0_V_TVALID));
  finn_grutail_GTDWR2_0 GTDWR2
       (.ap_clk(ap_clk_0_1),
        .ap_rst_n(ap_rst_n_0_1),
        .in0_V_TDATA(GTFFR10_out0_V_TDATA),
        .in0_V_TREADY(GTFFR10_out0_V_TREADY),
        .in0_V_TVALID(GTFFR10_out0_V_TVALID),
        .out0_V_TDATA(GTDWR2_out0_V_TDATA),
        .out0_V_TREADY(GTDWR2_out0_V_TREADY),
        .out0_V_TVALID(GTDWR2_out0_V_TVALID));
  finn_grutail_GTFFR0_0 GTFFR0
       (.ap_clk(ap_clk_0_1),
        .ap_rst_n(ap_rst_n_0_1),
        .in0_V_TDATA(in0_V_0_1_TDATA),
        .in0_V_TREADY(in0_V_0_1_TREADY),
        .in0_V_TVALID(in0_V_0_1_TVALID),
        .out0_V_TDATA(GTFFR0_out0_V_TDATA),
        .out0_V_TREADY(GTFFR0_out0_V_TREADY),
        .out0_V_TVALID(GTFFR0_out0_V_TVALID));
  finn_grutail_GTFFR1_0 GTFFR1
       (.ap_clk(ap_clk_0_1),
        .ap_rst_n(ap_rst_n_0_1),
        .in0_V_TDATA(GTISR0_out0_V_TDATA),
        .in0_V_TREADY(GTISR0_out0_V_TREADY),
        .in0_V_TVALID(GTISR0_out0_V_TVALID),
        .out0_V_TDATA(GTFFR1_out0_V_TDATA),
        .out0_V_TREADY(GTFFR1_out0_V_TREADY),
        .out0_V_TVALID(GTFFR1_out0_V_TVALID));
  finn_grutail_GTFFR10_0 GTFFR10
       (.ap_clk(ap_clk_0_1),
        .ap_rst_n(ap_rst_n_0_1),
        .in0_V_TDATA(GTTHR0_out0_V_TDATA),
        .in0_V_TREADY(GTTHR0_out0_V_TREADY),
        .in0_V_TVALID(GTTHR0_out0_V_TVALID),
        .out0_V_TDATA(GTFFR10_out0_V_TDATA),
        .out0_V_TREADY(GTFFR10_out0_V_TREADY),
        .out0_V_TVALID(GTFFR10_out0_V_TVALID));
  finn_grutail_GTFFR11_0 GTFFR11
       (.ap_clk(ap_clk_0_1),
        .ap_rst_n(ap_rst_n_0_1),
        .in0_V_TDATA(GTDWR2_out0_V_TDATA),
        .in0_V_TREADY(GTDWR2_out0_V_TREADY),
        .in0_V_TVALID(GTDWR2_out0_V_TVALID),
        .out0_V_TDATA(GTFFR11_out0_V_TDATA),
        .out0_V_TREADY(GTFFR11_out0_V_TREADY),
        .out0_V_TVALID(GTFFR11_out0_V_TVALID));
  finn_grutail_GTFFR12_0 GTFFR12
       (.ap_clk(ap_clk_0_1),
        .ap_rst_n(ap_rst_n_0_1),
        .in0_V_TDATA(GTISR1_out0_V_TDATA),
        .in0_V_TREADY(GTISR1_out0_V_TREADY),
        .in0_V_TVALID(GTISR1_out0_V_TVALID),
        .out0_V_TDATA(GTFFR12_out0_V_TDATA),
        .out0_V_TREADY(GTFFR12_out0_V_TREADY),
        .out0_V_TVALID(GTFFR12_out0_V_TVALID));
  finn_grutail_GTFFR2_0 GTFFR2
       (.ap_clk(ap_clk_0_1),
        .ap_rst_n(ap_rst_n_0_1),
        .in0_V_TDATA(GTISR0_out1_V_TDATA),
        .in0_V_TREADY(GTISR0_out1_V_TREADY),
        .in0_V_TVALID(GTISR0_out1_V_TVALID),
        .out0_V_TDATA(GTFFR2_out0_V_TDATA),
        .out0_V_TREADY(GTFFR2_out0_V_TREADY),
        .out0_V_TVALID(GTFFR2_out0_V_TVALID));
  finn_grutail_GTFFR3_0 GTFFR3
       (.ap_clk(ap_clk_0_1),
        .ap_rst_n(ap_rst_n_0_1),
        .in0_V_TDATA(GTISR0_out2_V_TDATA),
        .in0_V_TREADY(GTISR0_out2_V_TREADY),
        .in0_V_TVALID(GTISR0_out2_V_TVALID),
        .out0_V_TDATA(GTFFR3_out0_V_TDATA),
        .out0_V_TREADY(GTFFR3_out0_V_TREADY),
        .out0_V_TVALID(GTFFR3_out0_V_TVALID));
  finn_grutail_GTFFR4_0 GTFFR4
       (.ap_clk(ap_clk_0_1),
        .ap_rst_n(ap_rst_n_0_1),
        .in0_V_TDATA(GTIGR0_out0_V_TDATA),
        .in0_V_TREADY(GTIGR0_out0_V_TREADY),
        .in0_V_TVALID(GTIGR0_out0_V_TVALID),
        .out0_V_TDATA(GTFFR4_out0_V_TDATA),
        .out0_V_TREADY(GTFFR4_out0_V_TREADY),
        .out0_V_TVALID(GTFFR4_out0_V_TVALID));
  finn_grutail_GTFFR5_0 GTFFR5
       (.ap_clk(ap_clk_0_1),
        .ap_rst_n(ap_rst_n_0_1),
        .in0_V_TDATA(GTDWR0_out0_V_TDATA),
        .in0_V_TREADY(GTDWR0_out0_V_TREADY),
        .in0_V_TVALID(GTDWR0_out0_V_TVALID),
        .out0_V_TDATA(GTFFR5_out0_V_TDATA),
        .out0_V_TREADY(GTFFR5_out0_V_TREADY),
        .out0_V_TVALID(GTFFR5_out0_V_TVALID));
  finn_grutail_GTFFR6_0 GTFFR6
       (.ap_clk(ap_clk_0_1),
        .ap_rst_n(ap_rst_n_0_1),
        .in0_V_TDATA(GTDPH0_out1_V_V_TDATA[47:0]),
        .in0_V_TREADY(GTDPH0_out1_V_V_TREADY),
        .in0_V_TVALID(GTDPH0_out1_V_V_TVALID),
        .out0_V_TDATA(GTFFR6_out0_V_TDATA),
        .out0_V_TREADY(GTFFR6_out0_V_TREADY),
        .out0_V_TVALID(GTFFR6_out0_V_TVALID));
  finn_grutail_GTFFR7_0 GTFFR7
       (.ap_clk(ap_clk_0_1),
        .ap_rst_n(ap_rst_n_0_1),
        .in0_V_TDATA(GTDPH0_out0_V_V_TDATA[47:0]),
        .in0_V_TREADY(GTDPH0_out0_V_V_TREADY),
        .in0_V_TVALID(GTDPH0_out0_V_V_TVALID),
        .out0_V_TDATA(GTFFR7_out0_V_TDATA),
        .out0_V_TREADY(GTFFR7_out0_V_TREADY),
        .out0_V_TVALID(GTFFR7_out0_V_TVALID));
  finn_grutail_GTFFR8_0 GTFFR8
       (.ap_clk(ap_clk_0_1),
        .ap_rst_n(ap_rst_n_0_1),
        .in0_V_TDATA(GTMVH0_out0_V_TDATA),
        .in0_V_TREADY(GTMVH0_out0_V_TREADY),
        .in0_V_TVALID(GTMVH0_out0_V_TVALID),
        .out0_V_TDATA(GTFFR8_out0_V_TDATA),
        .out0_V_TREADY(GTFFR8_out0_V_TREADY),
        .out0_V_TVALID(GTFFR8_out0_V_TVALID));
  finn_grutail_GTFFR9_0 GTFFR9
       (.ap_clk(ap_clk_0_1),
        .ap_rst_n(ap_rst_n_0_1),
        .in0_V_TDATA(GTDWR1_out0_V_TDATA),
        .in0_V_TREADY(GTDWR1_out0_V_TREADY),
        .in0_V_TVALID(GTDWR1_out0_V_TVALID),
        .out0_V_TDATA(GTFFR9_out0_V_TDATA),
        .out0_V_TREADY(GTFFR9_out0_V_TREADY),
        .out0_V_TVALID(GTFFR9_out0_V_TVALID));
  finn_grutail_GTIGR0_0 GTIGR0
       (.ap_clk(ap_clk_0_1),
        .ap_rst_n(ap_rst_n_0_1),
        .in0_V_TDATA(GTFFR1_out0_V_TDATA),
        .in0_V_TREADY(GTFFR1_out0_V_TREADY),
        .in0_V_TVALID(GTFFR1_out0_V_TVALID),
        .in1_V_TDATA(GTFFR2_out0_V_TDATA),
        .in1_V_TREADY(GTFFR2_out0_V_TREADY),
        .in1_V_TVALID(GTFFR2_out0_V_TVALID),
        .in2_V_TDATA(GTFFR3_out0_V_TDATA),
        .in2_V_TREADY(GTFFR3_out0_V_TREADY),
        .in2_V_TVALID(GTFFR3_out0_V_TVALID),
        .out0_V_TDATA(GTIGR0_out0_V_TDATA),
        .out0_V_TREADY(GTIGR0_out0_V_TREADY),
        .out0_V_TVALID(GTIGR0_out0_V_TVALID),
        .s_axilite_ARADDR(s_axilite_0_1_ARADDR),
        .s_axilite_ARREADY(s_axilite_0_1_ARREADY),
        .s_axilite_ARVALID(s_axilite_0_1_ARVALID),
        .s_axilite_AWADDR(s_axilite_0_1_AWADDR),
        .s_axilite_AWREADY(s_axilite_0_1_AWREADY),
        .s_axilite_AWVALID(s_axilite_0_1_AWVALID),
        .s_axilite_BREADY(s_axilite_0_1_BREADY),
        .s_axilite_BRESP(s_axilite_0_1_BRESP),
        .s_axilite_BVALID(s_axilite_0_1_BVALID),
        .s_axilite_RDATA(s_axilite_0_1_RDATA),
        .s_axilite_RREADY(s_axilite_0_1_RREADY),
        .s_axilite_RRESP(s_axilite_0_1_RRESP),
        .s_axilite_RVALID(s_axilite_0_1_RVALID),
        .s_axilite_WDATA(s_axilite_0_1_WDATA),
        .s_axilite_WREADY(s_axilite_0_1_WREADY),
        .s_axilite_WSTRB(s_axilite_0_1_WSTRB),
        .s_axilite_WVALID(s_axilite_0_1_WVALID));
  finn_grutail_GTISR0_0 GTISR0
       (.ap_clk(ap_clk_0_1),
        .ap_rst_n(ap_rst_n_0_1),
        .in0_V_TDATA(GTFFR0_out0_V_TDATA),
        .in0_V_TREADY(GTFFR0_out0_V_TREADY),
        .in0_V_TVALID(GTFFR0_out0_V_TVALID),
        .out0_V_TDATA(GTISR0_out0_V_TDATA),
        .out0_V_TREADY(GTISR0_out0_V_TREADY),
        .out0_V_TVALID(GTISR0_out0_V_TVALID),
        .out1_V_TDATA(GTISR0_out1_V_TDATA),
        .out1_V_TREADY(GTISR0_out1_V_TREADY),
        .out1_V_TVALID(GTISR0_out1_V_TVALID),
        .out2_V_TDATA(GTISR0_out2_V_TDATA),
        .out2_V_TREADY(GTISR0_out2_V_TREADY),
        .out2_V_TVALID(GTISR0_out2_V_TVALID));
  finn_grutail_GTISR1_0 GTISR1
       (.ap_clk(ap_clk_0_1),
        .ap_rst_n(ap_rst_n_0_1),
        .in0_V_TDATA(GTFFR6_out0_V_TDATA),
        .in0_V_TREADY(GTFFR6_out0_V_TREADY),
        .in0_V_TVALID(GTFFR6_out0_V_TVALID),
        .in1_V_TDATA(GTFFR11_out0_V_TDATA),
        .in1_V_TREADY(GTFFR11_out0_V_TREADY),
        .in1_V_TVALID(GTFFR11_out0_V_TVALID),
        .out0_V_TDATA(GTISR1_out0_V_TDATA),
        .out0_V_TREADY(GTISR1_out0_V_TREADY),
        .out0_V_TVALID(GTISR1_out0_V_TVALID));
  GTMVH0_imp_YL0CMH GTMVH0
       (.ap_clk(ap_clk_0_1),
        .ap_rst_n(ap_rst_n_0_1),
        .in0_V_tdata(GTFFR7_out0_V_TDATA),
        .in0_V_tready(GTFFR7_out0_V_TREADY),
        .in0_V_tvalid(GTFFR7_out0_V_TVALID),
        .out0_V_tdata(GTMVH0_out0_V_TDATA),
        .out0_V_tready(GTMVH0_out0_V_TREADY),
        .out0_V_tvalid(GTMVH0_out0_V_TVALID),
        .s_axilite_araddr(s_axilite_1_1_ARADDR),
        .s_axilite_arprot(s_axilite_1_1_ARPROT),
        .s_axilite_arready(s_axilite_1_1_ARREADY),
        .s_axilite_arvalid(s_axilite_1_1_ARVALID),
        .s_axilite_awaddr(s_axilite_1_1_AWADDR),
        .s_axilite_awprot(s_axilite_1_1_AWPROT),
        .s_axilite_awready(s_axilite_1_1_AWREADY),
        .s_axilite_awvalid(s_axilite_1_1_AWVALID),
        .s_axilite_bready(s_axilite_1_1_BREADY),
        .s_axilite_bresp(s_axilite_1_1_BRESP),
        .s_axilite_bvalid(s_axilite_1_1_BVALID),
        .s_axilite_rdata(s_axilite_1_1_RDATA),
        .s_axilite_rready(s_axilite_1_1_RREADY),
        .s_axilite_rresp(s_axilite_1_1_RRESP),
        .s_axilite_rvalid(s_axilite_1_1_RVALID),
        .s_axilite_wdata(s_axilite_1_1_WDATA),
        .s_axilite_wready(s_axilite_1_1_WREADY),
        .s_axilite_wstrb(s_axilite_1_1_WSTRB),
        .s_axilite_wvalid(s_axilite_1_1_WVALID));
  finn_grutail_GTTHR0_0 GTTHR0
       (.ap_clk(ap_clk_0_1),
        .ap_rst_n(ap_rst_n_0_1),
        .in0_V_TDATA(GTFFR9_out0_V_TDATA),
        .in0_V_TREADY(GTFFR9_out0_V_TREADY),
        .in0_V_TVALID(GTFFR9_out0_V_TVALID),
        .in1_V_TDATA({1'b0,1'b0,1'b0,1'b0,1'b0,1'b0,1'b0,1'b0}),
        .in1_V_TVALID(1'b0),
        .out0_V_TDATA(GTTHR0_out0_V_TDATA),
        .out0_V_TREADY(GTTHR0_out0_V_TREADY),
        .out0_V_TVALID(GTTHR0_out0_V_TVALID),
        .s_axilite_ARADDR(s_axilite_2_1_ARADDR),
        .s_axilite_ARREADY(s_axilite_2_1_ARREADY),
        .s_axilite_ARVALID(s_axilite_2_1_ARVALID),
        .s_axilite_AWADDR(s_axilite_2_1_AWADDR),
        .s_axilite_AWREADY(s_axilite_2_1_AWREADY),
        .s_axilite_AWVALID(s_axilite_2_1_AWVALID),
        .s_axilite_BREADY(s_axilite_2_1_BREADY),
        .s_axilite_BRESP(s_axilite_2_1_BRESP),
        .s_axilite_BVALID(s_axilite_2_1_BVALID),
        .s_axilite_RDATA(s_axilite_2_1_RDATA),
        .s_axilite_RREADY(s_axilite_2_1_RREADY),
        .s_axilite_RRESP(s_axilite_2_1_RRESP),
        .s_axilite_RVALID(s_axilite_2_1_RVALID),
        .s_axilite_WDATA(s_axilite_2_1_WDATA),
        .s_axilite_WREADY(s_axilite_2_1_WREADY),
        .s_axilite_WSTRB(s_axilite_2_1_WSTRB),
        .s_axilite_WVALID(s_axilite_2_1_WVALID));
  finn_grutail_sim_ctrl_0_0 sim_ctrl_0
       (.ap_clk(ap_clk_0_1),
        .sim_finish(sim_finish_0_1));
endmodule
