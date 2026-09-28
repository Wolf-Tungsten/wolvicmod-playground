// WolvicZjBB：wolvicmod ZhuJiang L3 模型（WolvicZjTop）的 Verilog DPI-C 薄壳。
//
// 端口与 chisel BlackBox（top/WolvicZjBB.scala）逐叶一致；经 verilator libdir
// （make emu RTL_INCLUDE=<本目录>，difftest handle_rtl_include_path → -y）注入。
//
// 时序方案（单调用，前提 = 模型边界零组合穿透，proj tests/test_comb_audit 常驻
// 审计）：每个 posedge 调一次 wolvic_zj_step——C++ 侧以边沿前输入采样驱动模型、
// clk 0→1 提交状态、返回新输出到 t_out；o_out <= t_out 经 NBA 提交，act region
// 内边界输出保持边沿前值，与全寄存边界语义一致。复位期间不调用（模型构造态 =
// 复位完成态，与 P4b golden trace 对齐语义相同）；initial 调 wolvic_zj_peek
// 取初态输出。
//
// 打包约定（in_concat / WolvicZjOut 与 dpi/csrc/wolvic_zj_dpi.cpp 的
// pack/unpack 顺序一一对应，改动必须双侧同步）：
//   - CHI 通道 bits 按模型 pack() 布局（model/flit/xs_flit.h：声明序、先声明
//     占高位）：CHIREQ 118b / CHIRSP 66b / CHIDAT 367b / CHISNP 102b；
//   - 被模型裁剪的 BlackBox 字段（REQ 的 returnNID/stashNIDValid/returnTxnID/
//     ns/likelyshared/allowRetry/pCrdType/lpIDWithPadding/tagOp/traceTag/
//     mpam_{perfMonGroup,mpamNS}、DAT 的 ccID/tagOp/tag/tu/traceTag/rsvdc、
//     SNP 的 ns/traceTag/mpam_*）输入侧丢弃、输出侧绑 0——与 ZhuJiangBridge
//     map* 的读取集一致。

import "DPI-C" function void wolvic_zj_step(
    input  bit [1106:0] in_pack,
    output bit [1442:0] out_pack
);

import "DPI-C" function void wolvic_zj_peek(
    output bit [1442:0] out_pack
);

module WolvicZjBB (
    input              clock,
    input              reset,
    // rn[0] tx（tile → L3）
    output             rn_0_tx_req_ready,
    input              rn_0_tx_req_valid,
    input  [3:0]       rn_0_tx_req_bits_qos,
    input  [10:0]      rn_0_tx_req_bits_tgtID,
    input  [10:0]      rn_0_tx_req_bits_srcID,
    input  [11:0]      rn_0_tx_req_bits_txnID,
    input  [10:0]      rn_0_tx_req_bits_returnNID,
    input              rn_0_tx_req_bits_stashNIDValid,
    input  [11:0]      rn_0_tx_req_bits_returnTxnID,
    input  [6:0]       rn_0_tx_req_bits_opcode,
    input  [2:0]       rn_0_tx_req_bits_size,
    input  [47:0]      rn_0_tx_req_bits_addr,
    input              rn_0_tx_req_bits_ns,
    input              rn_0_tx_req_bits_likelyshared,
    input              rn_0_tx_req_bits_allowRetry,
    input  [1:0]       rn_0_tx_req_bits_order,
    input  [3:0]       rn_0_tx_req_bits_pCrdType,
    input              rn_0_tx_req_bits_memAttr_allocate,
    input              rn_0_tx_req_bits_memAttr_cacheable,
    input              rn_0_tx_req_bits_memAttr_device,
    input              rn_0_tx_req_bits_memAttr_ewa,
    input              rn_0_tx_req_bits_snpAttr,
    input  [7:0]       rn_0_tx_req_bits_lpIDWithPadding,
    input              rn_0_tx_req_bits_snoopMe,
    input              rn_0_tx_req_bits_expCompAck,
    input  [1:0]       rn_0_tx_req_bits_tagOp,
    input              rn_0_tx_req_bits_traceTag,
    input              rn_0_tx_req_bits_mpam_perfMonGroup,
    input  [8:0]       rn_0_tx_req_bits_mpam_partID,
    input              rn_0_tx_req_bits_mpam_mpamNS,
    input  [3:0]       rn_0_tx_req_bits_rsvdc,
    output             rn_0_tx_rsp_ready,
    input              rn_0_tx_rsp_valid,
    input  [3:0]       rn_0_tx_rsp_bits_qos,
    input  [10:0]      rn_0_tx_rsp_bits_tgtID,
    input  [10:0]      rn_0_tx_rsp_bits_srcID,
    input  [11:0]      rn_0_tx_rsp_bits_txnID,
    input  [4:0]       rn_0_tx_rsp_bits_opcode,
    input  [1:0]       rn_0_tx_rsp_bits_respErr,
    input  [2:0]       rn_0_tx_rsp_bits_resp,
    input  [2:0]       rn_0_tx_rsp_bits_fwdState,
    input  [2:0]       rn_0_tx_rsp_bits_cBusy,
    input  [11:0]      rn_0_tx_rsp_bits_dbID,
    input  [3:0]       rn_0_tx_rsp_bits_pCrdType,
    input  [1:0]       rn_0_tx_rsp_bits_tagOp,
    input              rn_0_tx_rsp_bits_traceTag,
    output             rn_0_tx_dat_ready,
    input              rn_0_tx_dat_valid,
    input  [3:0]       rn_0_tx_dat_bits_qos,
    input  [10:0]      rn_0_tx_dat_bits_tgtID,
    input  [10:0]      rn_0_tx_dat_bits_srcID,
    input  [11:0]      rn_0_tx_dat_bits_txnID,
    input  [10:0]      rn_0_tx_dat_bits_homeNID,
    input  [3:0]       rn_0_tx_dat_bits_opcode,
    input  [1:0]       rn_0_tx_dat_bits_respErr,
    input  [2:0]       rn_0_tx_dat_bits_resp,
    input  [3:0]       rn_0_tx_dat_bits_dataSource,
    input  [2:0]       rn_0_tx_dat_bits_cBusy,
    input  [11:0]      rn_0_tx_dat_bits_dbID,
    input  [1:0]       rn_0_tx_dat_bits_ccID,
    input  [1:0]       rn_0_tx_dat_bits_dataID,
    input  [1:0]       rn_0_tx_dat_bits_tagOp,
    input  [7:0]       rn_0_tx_dat_bits_tag,
    input  [1:0]       rn_0_tx_dat_bits_tu,
    input              rn_0_tx_dat_bits_traceTag,
    input  [3:0]       rn_0_tx_dat_bits_rsvdc,
    input  [31:0]      rn_0_tx_dat_bits_be,
    input  [255:0]     rn_0_tx_dat_bits_data,
    // rn[0] rx（L3 → tile）
    input              rn_0_rx_rsp_ready,
    output             rn_0_rx_rsp_valid,
    output [3:0]       rn_0_rx_rsp_bits_qos,
    output [10:0]      rn_0_rx_rsp_bits_tgtID,
    output [10:0]      rn_0_rx_rsp_bits_srcID,
    output [11:0]      rn_0_rx_rsp_bits_txnID,
    output [4:0]       rn_0_rx_rsp_bits_opcode,
    output [1:0]       rn_0_rx_rsp_bits_respErr,
    output [2:0]       rn_0_rx_rsp_bits_resp,
    output [2:0]       rn_0_rx_rsp_bits_fwdState,
    output [2:0]       rn_0_rx_rsp_bits_cBusy,
    output [11:0]      rn_0_rx_rsp_bits_dbID,
    output [3:0]       rn_0_rx_rsp_bits_pCrdType,
    output [1:0]       rn_0_rx_rsp_bits_tagOp,
    output             rn_0_rx_rsp_bits_traceTag,
    input              rn_0_rx_dat_ready,
    output             rn_0_rx_dat_valid,
    output [3:0]       rn_0_rx_dat_bits_qos,
    output [10:0]      rn_0_rx_dat_bits_tgtID,
    output [10:0]      rn_0_rx_dat_bits_srcID,
    output [11:0]      rn_0_rx_dat_bits_txnID,
    output [10:0]      rn_0_rx_dat_bits_homeNID,
    output [3:0]       rn_0_rx_dat_bits_opcode,
    output [1:0]       rn_0_rx_dat_bits_respErr,
    output [2:0]       rn_0_rx_dat_bits_resp,
    output [3:0]       rn_0_rx_dat_bits_dataSource,
    output [2:0]       rn_0_rx_dat_bits_cBusy,
    output [11:0]      rn_0_rx_dat_bits_dbID,
    output [1:0]       rn_0_rx_dat_bits_ccID,
    output [1:0]       rn_0_rx_dat_bits_dataID,
    output [1:0]       rn_0_rx_dat_bits_tagOp,
    output [7:0]       rn_0_rx_dat_bits_tag,
    output [1:0]       rn_0_rx_dat_bits_tu,
    output             rn_0_rx_dat_bits_traceTag,
    output [3:0]       rn_0_rx_dat_bits_rsvdc,
    output [31:0]      rn_0_rx_dat_bits_be,
    output [255:0]     rn_0_rx_dat_bits_data,
    input              rn_0_rx_snp_ready,
    output             rn_0_rx_snp_valid,
    output [3:0]       rn_0_rx_snp_bits_qos,
    output [10:0]      rn_0_rx_snp_bits_srcID,
    output [11:0]      rn_0_rx_snp_bits_txnID,
    output [10:0]      rn_0_rx_snp_bits_fwdNID,
    output [11:0]      rn_0_rx_snp_bits_fwdTxnID,
    output [4:0]       rn_0_rx_snp_bits_opcode,
    output [44:0]      rn_0_rx_snp_bits_addr,
    output             rn_0_rx_snp_bits_ns,
    output             rn_0_rx_snp_bits_doNotGoToSD,
    output             rn_0_rx_snp_bits_retToSrc,
    output             rn_0_rx_snp_bits_traceTag,
    output             rn_0_rx_snp_bits_mpam_perfMonGroup,
    output [8:0]       rn_0_rx_snp_bits_mpam_partID,
    output             rn_0_rx_snp_bits_mpam_mpamNS,
    // ddrc（memAXI）
    input              ddrc_awready,
    output             ddrc_awvalid,
    output [5:0]       ddrc_awid,
    output [48:0]      ddrc_awaddr,
    output [7:0]       ddrc_awlen,
    output [2:0]       ddrc_awsize,
    output [1:0]       ddrc_awburst,
    output             ddrc_awlock,
    output [3:0]       ddrc_awcache,
    output [2:0]       ddrc_awprot,
    output [3:0]       ddrc_awqos,
    input              ddrc_wready,
    output             ddrc_wvalid,
    output [255:0]     ddrc_wdata,
    output [31:0]      ddrc_wstrb,
    output             ddrc_wlast,
    output             ddrc_bready,
    input              ddrc_bvalid,
    input  [5:0]       ddrc_bid,
    input  [1:0]       ddrc_bresp,
    input              ddrc_arready,
    output             ddrc_arvalid,
    output [5:0]       ddrc_arid,
    output [48:0]      ddrc_araddr,
    output [7:0]       ddrc_arlen,
    output [2:0]       ddrc_arsize,
    output [1:0]       ddrc_arburst,
    output             ddrc_arlock,
    output [3:0]       ddrc_arcache,
    output [2:0]       ddrc_arprot,
    output [3:0]       ddrc_arqos,
    output             ddrc_rready,
    input              ddrc_rvalid,
    input  [5:0]       ddrc_rid,
    input  [255:0]     ddrc_rdata,
    input  [1:0]       ddrc_rresp,
    input              ddrc_rlast,
    // peri[0]（cfgAXI）
    input              peri_0_awready,
    output             peri_0_awvalid,
    output [2:0]       peri_0_awid,
    output [48:0]      peri_0_awaddr,
    output [7:0]       peri_0_awlen,
    output [2:0]       peri_0_awsize,
    output [1:0]       peri_0_awburst,
    output             peri_0_awlock,
    output [3:0]       peri_0_awcache,
    output [2:0]       peri_0_awprot,
    output [3:0]       peri_0_awqos,
    input              peri_0_wready,
    output             peri_0_wvalid,
    output [255:0]     peri_0_wdata,
    output [31:0]      peri_0_wstrb,
    output             peri_0_wlast,
    output             peri_0_bready,
    input              peri_0_bvalid,
    input  [2:0]       peri_0_bid,
    input  [1:0]       peri_0_bresp,
    input              peri_0_arready,
    output             peri_0_arvalid,
    output [2:0]       peri_0_arid,
    output [48:0]      peri_0_araddr,
    output [7:0]       peri_0_arlen,
    output [2:0]       peri_0_arsize,
    output [1:0]       peri_0_arburst,
    output             peri_0_arlock,
    output [3:0]       peri_0_arcache,
    output [2:0]       peri_0_arprot,
    output [3:0]       peri_0_arqos,
    output             peri_0_rready,
    input              peri_0_rvalid,
    input  [2:0]       peri_0_rid,
    input  [255:0]     peri_0_rdata,
    input  [1:0]       peri_0_rresp,
    input              peri_0_rlast
);

    // 模型输出包（声明序 = pack 序：先声明占高位）。与 C++ packOutputs 对应。
    typedef struct packed {
        // L2 CHI 输出
        logic         tx_req_ready;
        logic         tx_rsp_ready;
        logic         tx_dat_ready;
        logic         rx_rsp_valid;
        logic [65:0]  rx_rsp_bits;
        logic         rx_dat_valid;
        logic [366:0] rx_dat_bits;
        logic         rx_snp_valid;
        logic [101:0] rx_snp_bits;
        // memAXI 输出
        logic         mem_awvalid;
        logic [5:0]   mem_awid;
        logic [48:0]  mem_awaddr;
        logic [7:0]   mem_awlen;
        logic [2:0]   mem_awsize;
        logic [1:0]   mem_awburst;
        logic         mem_awlock;
        logic [3:0]   mem_awcache;
        logic [2:0]   mem_awprot;
        logic [3:0]   mem_awqos;
        logic         mem_wvalid;
        logic [255:0] mem_wdata;
        logic [31:0]  mem_wstrb;
        logic         mem_wlast;
        logic         mem_bready;
        logic         mem_arvalid;
        logic [5:0]   mem_arid;
        logic [48:0]  mem_araddr;
        logic [7:0]   mem_arlen;
        logic [2:0]   mem_arsize;
        logic [1:0]   mem_arburst;
        logic         mem_arlock;
        logic [3:0]   mem_arcache;
        logic [2:0]   mem_arprot;
        logic [3:0]   mem_arqos;
        logic         mem_rready;
        // cfgAXI 输出
        logic         cfg_awvalid;
        logic [2:0]   cfg_awid;
        logic [48:0]  cfg_awaddr;
        logic [7:0]   cfg_awlen;
        logic [2:0]   cfg_awsize;
        logic [1:0]   cfg_awburst;
        logic         cfg_awlock;
        logic [3:0]   cfg_awcache;
        logic [2:0]   cfg_awprot;
        logic [3:0]   cfg_awqos;
        logic         cfg_wvalid;
        logic [255:0] cfg_wdata;
        logic [31:0]  cfg_wstrb;
        logic         cfg_wlast;
        logic         cfg_bready;
        logic         cfg_arvalid;
        logic [2:0]   cfg_arid;
        logic [48:0]  cfg_araddr;
        logic [7:0]   cfg_arlen;
        logic [2:0]   cfg_arsize;
        logic [1:0]   cfg_arburst;
        logic         cfg_arlock;
        logic [3:0]   cfg_arcache;
        logic [2:0]   cfg_arprot;
        logic [3:0]   cfg_arqos;
        logic         cfg_rready;
    } WolvicZjOut;

    initial begin
        if ($bits(WolvicZjOut) != 1443) $fatal(1, "WolvicZjOut width drift");
    end

    // ---- CHI bits 打包（模型 pack() 布局：先声明占高位） ----
    wire [117:0] tx_req_bits = {
        rn_0_tx_req_bits_qos, rn_0_tx_req_bits_tgtID, rn_0_tx_req_bits_srcID,
        rn_0_tx_req_bits_txnID, rn_0_tx_req_bits_opcode, rn_0_tx_req_bits_size,
        rn_0_tx_req_bits_addr, rn_0_tx_req_bits_order,
        rn_0_tx_req_bits_memAttr_allocate, rn_0_tx_req_bits_memAttr_cacheable,
        rn_0_tx_req_bits_memAttr_device, rn_0_tx_req_bits_memAttr_ewa,
        rn_0_tx_req_bits_snpAttr, rn_0_tx_req_bits_snoopMe,
        rn_0_tx_req_bits_expCompAck, rn_0_tx_req_bits_mpam_partID,
        rn_0_tx_req_bits_rsvdc
    };
    wire [65:0] tx_rsp_bits = {
        rn_0_tx_rsp_bits_qos, rn_0_tx_rsp_bits_tgtID, rn_0_tx_rsp_bits_srcID,
        rn_0_tx_rsp_bits_txnID, rn_0_tx_rsp_bits_opcode, rn_0_tx_rsp_bits_respErr,
        rn_0_tx_rsp_bits_resp, rn_0_tx_rsp_bits_fwdState, rn_0_tx_rsp_bits_cBusy,
        rn_0_tx_rsp_bits_dbID
    };
    wire [366:0] tx_dat_bits = {
        rn_0_tx_dat_bits_qos, rn_0_tx_dat_bits_tgtID, rn_0_tx_dat_bits_srcID,
        rn_0_tx_dat_bits_txnID, rn_0_tx_dat_bits_homeNID, rn_0_tx_dat_bits_opcode,
        rn_0_tx_dat_bits_respErr, rn_0_tx_dat_bits_resp, rn_0_tx_dat_bits_dataSource,
        rn_0_tx_dat_bits_cBusy, rn_0_tx_dat_bits_dbID, rn_0_tx_dat_bits_dataID,
        rn_0_tx_dat_bits_be, rn_0_tx_dat_bits_data
    };

    // ---- 输入打包（顺序与 C++ unpackInputs 对应：先占高位） ----
    wire [1106:0] in_pack = {
        rn_0_tx_req_valid, tx_req_bits,
        rn_0_tx_rsp_valid, tx_rsp_bits,
        rn_0_tx_dat_valid, tx_dat_bits,
        rn_0_rx_rsp_ready, rn_0_rx_dat_ready, rn_0_rx_snp_ready,
        ddrc_awready, ddrc_wready,
        ddrc_bvalid, ddrc_bid, ddrc_bresp,
        ddrc_arready,
        ddrc_rvalid, ddrc_rid, ddrc_rdata, ddrc_rresp, ddrc_rlast,
        peri_0_awready, peri_0_wready,
        peri_0_bvalid, peri_0_bid, peri_0_bresp,
        peri_0_arready,
        peri_0_rvalid, peri_0_rid, peri_0_rdata, peri_0_rresp, peri_0_rlast
    };

    WolvicZjOut t_out;  // DPI 返回暂存（act region 内被写）

    // 输出寄存器按通道拆分（valid 与 bits 再拆开），而非单个 1443b 结构体：
    // 机制：Verilator 的 NBA 提交按**变量粒度**做变化检测并触发下游组合重估——
    // 单体结构下任何字段变化都唤醒全部输出消费者（每拍全边界 settle）；
    // 拆分后只有值真正变化的通道触发自己的下游锥，静止通道（如 boot 后的
    // cfgAXI、多数拍无传输的 mem 通道）的下游逻辑整片跳过。
    logic         tx_req_ready_q, tx_rsp_ready_q, tx_dat_ready_q;
    logic         rx_rsp_valid_q;
    logic [65:0]  rx_rsp_bits_q;
    logic         rx_dat_valid_q;
    logic [366:0] rx_dat_bits_q;
    logic         rx_snp_valid_q;
    logic [101:0] rx_snp_bits_q;
    logic         mem_awvalid_q, mem_wvalid_q, mem_bready_q, mem_arvalid_q, mem_rready_q;
    logic [79:0]  mem_aw_bits_q;   // id+addr+len+size+burst+lock+cache+prot+qos
    logic [288:0] mem_w_bits_q;    // data+strb+last
    logic [79:0]  mem_ar_bits_q;
    logic         cfg_awvalid_q, cfg_wvalid_q, cfg_bready_q, cfg_arvalid_q, cfg_rready_q;
    logic [76:0]  cfg_aw_bits_q;   // id 比 mem 侧窄 3 位
    logic [288:0] cfg_w_bits_q;
    logic [76:0]  cfg_ar_bits_q;

    initial begin
        wolvic_zj_peek(t_out);
        tx_req_ready_q = t_out.tx_req_ready;
        tx_rsp_ready_q = t_out.tx_rsp_ready;
        tx_dat_ready_q = t_out.tx_dat_ready;
        rx_rsp_valid_q = t_out.rx_rsp_valid;
        rx_rsp_bits_q  = t_out.rx_rsp_bits;
        rx_dat_valid_q = t_out.rx_dat_valid;
        rx_dat_bits_q  = t_out.rx_dat_bits;
        rx_snp_valid_q = t_out.rx_snp_valid;
        rx_snp_bits_q  = t_out.rx_snp_bits;
        mem_awvalid_q  = t_out.mem_awvalid;
        mem_aw_bits_q  = {t_out.mem_awid, t_out.mem_awaddr, t_out.mem_awlen,
                          t_out.mem_awsize, t_out.mem_awburst, t_out.mem_awlock,
                          t_out.mem_awcache, t_out.mem_awprot, t_out.mem_awqos};
        mem_wvalid_q   = t_out.mem_wvalid;
        mem_w_bits_q   = {t_out.mem_wdata, t_out.mem_wstrb, t_out.mem_wlast};
        mem_bready_q   = t_out.mem_bready;
        mem_arvalid_q  = t_out.mem_arvalid;
        mem_ar_bits_q  = {t_out.mem_arid, t_out.mem_araddr, t_out.mem_arlen,
                          t_out.mem_arsize, t_out.mem_arburst, t_out.mem_arlock,
                          t_out.mem_arcache, t_out.mem_arprot, t_out.mem_arqos};
        mem_rready_q   = t_out.mem_rready;
        cfg_awvalid_q  = t_out.cfg_awvalid;
        cfg_aw_bits_q  = {t_out.cfg_awid, t_out.cfg_awaddr, t_out.cfg_awlen,
                          t_out.cfg_awsize, t_out.cfg_awburst, t_out.cfg_awlock,
                          t_out.cfg_awcache, t_out.cfg_awprot, t_out.cfg_awqos};
        cfg_wvalid_q   = t_out.cfg_wvalid;
        cfg_w_bits_q   = {t_out.cfg_wdata, t_out.cfg_wstrb, t_out.cfg_wlast};
        cfg_bready_q   = t_out.cfg_bready;
        cfg_arvalid_q  = t_out.cfg_arvalid;
        cfg_ar_bits_q  = {t_out.cfg_arid, t_out.cfg_araddr, t_out.cfg_arlen,
                          t_out.cfg_arsize, t_out.cfg_arburst, t_out.cfg_arlock,
                          t_out.cfg_arcache, t_out.cfg_arprot, t_out.cfg_arqos};
        cfg_rready_q   = t_out.cfg_rready;
    end

    always @(posedge clock) begin
        if (!reset) begin
            wolvic_zj_step(in_pack, t_out);
            tx_req_ready_q <= t_out.tx_req_ready;
            tx_rsp_ready_q <= t_out.tx_rsp_ready;
            tx_dat_ready_q <= t_out.tx_dat_ready;
            rx_rsp_valid_q <= t_out.rx_rsp_valid;
            rx_rsp_bits_q  <= t_out.rx_rsp_bits;
            rx_dat_valid_q <= t_out.rx_dat_valid;
            rx_dat_bits_q  <= t_out.rx_dat_bits;
            rx_snp_valid_q <= t_out.rx_snp_valid;
            rx_snp_bits_q  <= t_out.rx_snp_bits;
            mem_awvalid_q  <= t_out.mem_awvalid;
            mem_aw_bits_q  <= {t_out.mem_awid, t_out.mem_awaddr, t_out.mem_awlen,
                               t_out.mem_awsize, t_out.mem_awburst, t_out.mem_awlock,
                               t_out.mem_awcache, t_out.mem_awprot, t_out.mem_awqos};
            mem_wvalid_q   <= t_out.mem_wvalid;
            mem_w_bits_q   <= {t_out.mem_wdata, t_out.mem_wstrb, t_out.mem_wlast};
            mem_bready_q   <= t_out.mem_bready;
            mem_arvalid_q  <= t_out.mem_arvalid;
            mem_ar_bits_q  <= {t_out.mem_arid, t_out.mem_araddr, t_out.mem_arlen,
                               t_out.mem_arsize, t_out.mem_arburst, t_out.mem_arlock,
                               t_out.mem_arcache, t_out.mem_arprot, t_out.mem_arqos};
            mem_rready_q   <= t_out.mem_rready;
            cfg_awvalid_q  <= t_out.cfg_awvalid;
            cfg_aw_bits_q  <= {t_out.cfg_awid, t_out.cfg_awaddr, t_out.cfg_awlen,
                               t_out.cfg_awsize, t_out.cfg_awburst, t_out.cfg_awlock,
                               t_out.cfg_awcache, t_out.cfg_awprot, t_out.cfg_awqos};
            cfg_wvalid_q   <= t_out.cfg_wvalid;
            cfg_w_bits_q   <= {t_out.cfg_wdata, t_out.cfg_wstrb, t_out.cfg_wlast};
            cfg_bready_q   <= t_out.cfg_bready;
            cfg_arvalid_q  <= t_out.cfg_arvalid;
            cfg_ar_bits_q  <= {t_out.cfg_arid, t_out.cfg_araddr, t_out.cfg_arlen,
                               t_out.cfg_arsize, t_out.cfg_arburst, t_out.cfg_arlock,
                               t_out.cfg_arcache, t_out.cfg_arprot, t_out.cfg_arqos};
            cfg_rready_q   <= t_out.cfg_rready;
        end
    end

    // ---- 输出驱动 ----
    assign rn_0_tx_req_ready = tx_req_ready_q;
    assign rn_0_tx_rsp_ready = tx_rsp_ready_q;
    assign rn_0_tx_dat_ready = tx_dat_ready_q;

    assign rn_0_rx_rsp_valid         = rx_rsp_valid_q;
    assign {rn_0_rx_rsp_bits_qos, rn_0_rx_rsp_bits_tgtID, rn_0_rx_rsp_bits_srcID,
            rn_0_rx_rsp_bits_txnID, rn_0_rx_rsp_bits_opcode, rn_0_rx_rsp_bits_respErr,
            rn_0_rx_rsp_bits_resp, rn_0_rx_rsp_bits_fwdState, rn_0_rx_rsp_bits_cBusy,
            rn_0_rx_rsp_bits_dbID}   = rx_rsp_bits_q;
    assign rn_0_rx_rsp_bits_pCrdType = 4'b0;
    assign rn_0_rx_rsp_bits_tagOp    = 2'b0;
    assign rn_0_rx_rsp_bits_traceTag = 1'b0;

    assign rn_0_rx_dat_valid         = rx_dat_valid_q;
    assign {rn_0_rx_dat_bits_qos, rn_0_rx_dat_bits_tgtID, rn_0_rx_dat_bits_srcID,
            rn_0_rx_dat_bits_txnID, rn_0_rx_dat_bits_homeNID, rn_0_rx_dat_bits_opcode,
            rn_0_rx_dat_bits_respErr, rn_0_rx_dat_bits_resp, rn_0_rx_dat_bits_dataSource,
            rn_0_rx_dat_bits_cBusy, rn_0_rx_dat_bits_dbID, rn_0_rx_dat_bits_dataID,
            rn_0_rx_dat_bits_be, rn_0_rx_dat_bits_data} = rx_dat_bits_q;
    assign rn_0_rx_dat_bits_ccID     = 2'b0;
    assign rn_0_rx_dat_bits_tagOp    = 2'b0;
    assign rn_0_rx_dat_bits_tag      = 8'b0;
    assign rn_0_rx_dat_bits_tu       = 2'b0;
    assign rn_0_rx_dat_bits_traceTag = 1'b0;
    assign rn_0_rx_dat_bits_rsvdc    = 4'b0;

    assign rn_0_rx_snp_valid         = rx_snp_valid_q;
    assign {rn_0_rx_snp_bits_qos, rn_0_rx_snp_bits_srcID, rn_0_rx_snp_bits_txnID,
            rn_0_rx_snp_bits_fwdNID, rn_0_rx_snp_bits_fwdTxnID, rn_0_rx_snp_bits_opcode,
            rn_0_rx_snp_bits_addr, rn_0_rx_snp_bits_doNotGoToSD,
            rn_0_rx_snp_bits_retToSrc} = rx_snp_bits_q;
    assign rn_0_rx_snp_bits_ns                = 1'b0;
    assign rn_0_rx_snp_bits_traceTag          = 1'b0;
    assign rn_0_rx_snp_bits_mpam_perfMonGroup = 1'b0;
    assign rn_0_rx_snp_bits_mpam_partID       = 9'b0;
    assign rn_0_rx_snp_bits_mpam_mpamNS       = 1'b0;

    assign {ddrc_awid, ddrc_awaddr, ddrc_awlen, ddrc_awsize, ddrc_awburst,
            ddrc_awlock, ddrc_awcache, ddrc_awprot, ddrc_awqos} = mem_aw_bits_q;
    assign ddrc_awvalid = mem_awvalid_q;
    assign {ddrc_wdata, ddrc_wstrb, ddrc_wlast} = mem_w_bits_q;
    assign ddrc_wvalid  = mem_wvalid_q;
    assign ddrc_bready  = mem_bready_q;
    assign {ddrc_arid, ddrc_araddr, ddrc_arlen, ddrc_arsize, ddrc_arburst,
            ddrc_arlock, ddrc_arcache, ddrc_arprot, ddrc_arqos} = mem_ar_bits_q;
    assign ddrc_arvalid = mem_arvalid_q;
    assign ddrc_rready  = mem_rready_q;

    assign {peri_0_awid, peri_0_awaddr, peri_0_awlen, peri_0_awsize, peri_0_awburst,
            peri_0_awlock, peri_0_awcache, peri_0_awprot, peri_0_awqos} = cfg_aw_bits_q;
    assign peri_0_awvalid = cfg_awvalid_q;
    assign {peri_0_wdata, peri_0_wstrb, peri_0_wlast} = cfg_w_bits_q;
    assign peri_0_wvalid  = cfg_wvalid_q;
    assign peri_0_bready  = cfg_bready_q;
    assign {peri_0_arid, peri_0_araddr, peri_0_arlen, peri_0_arsize, peri_0_arburst,
            peri_0_arlock, peri_0_arcache, peri_0_arprot, peri_0_arqos} = cfg_ar_bits_q;
    assign peri_0_arvalid = cfg_arvalid_q;
    assign peri_0_rready  = cfg_rready_q;

endmodule
