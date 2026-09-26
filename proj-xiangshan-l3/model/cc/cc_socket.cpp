#include "model/cc/cc_socket.h"

#include <wolvicmod/wolvicmod.h>

namespace zj::sock {

CcSocket::CcSocket() {
    buildChan(l2_rx_req, l2_rx_req_rdy, ring_rx_req, ring_rx_req_rdy, ij_req_tx, ij_req_rx);
    buildChan(l2_rx_resp, l2_rx_resp_rdy, ring_rx_resp, ring_rx_resp_rdy, ij_rsp_tx, ij_rsp_rx);
    buildChan(l2_rx_data, l2_rx_data_rdy, ring_rx_data, ring_rx_data_rdy, ij_dat_tx, ij_dat_rx);
    buildChan(ring_tx_req, ring_tx_req_rdy, l2_tx_req, l2_tx_req_rdy, ej_req_tx, ej_req_rx);
    buildChan(ring_tx_resp, ring_tx_resp_rdy, l2_tx_resp, l2_tx_resp_rdy, ej_rsp_tx, ej_rsp_rx);
    buildChan(ring_tx_data, ring_tx_data_rdy, l2_tx_data, l2_tx_data_rdy, ej_dat_tx, ej_dat_rx);
    buildChan(ring_tx_snoop, ring_tx_snoop_rdy, l2_tx_snoop, l2_tx_snoop_rdy, ej_snp_tx, ej_snp_rx);
}

}  // namespace zj::sock
