#include "dtls.h"

extern "C" {
#include <glue.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <net/ethernet.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>

#include <datapath.h>
struct rmlock { uint8_t pad; }; /* Needed by pfil.h */
#include <net/pfil.h> /* PFIL_IN */
}

#include <nmutil.h>
#include "command.h"
#include "uvxbridge.h"
#include "uvxlan.h"
extern int debug;


void
uvxcmd_fill(char *txbuf, uint64_t smac, uint64_t dmac, uint16_t op, uint16_t rc, uint16_t seqno)
{
	struct ether_header *eh = (struct ether_header *)txbuf;
	struct uvxcmd_header *uh = (struct uvxcmd_header *)(eh + 1);

	eh_fill(eh, smac, dmac, ETHERTYPE_UVXCONF);
	uh->uh_magic = UVXMAGIC;
	uh->uh_op = op;
	uh->uh_rc = rc;
	uh->uh_seqno = seqno;
}

int
cmd_initiate(char *rxbuf __unused, char *txbuf, path_state_t *ps, void *arg)
{
	vxstate_t *oldstate, *newstate, *state = (vxstate_t *)arg;
	rte_t *rte = &state->vs_dflt_rte;
	struct timeval tnow, delta;
	int dp_count, count = 0;
	uint16_t op;

	gettimeofday(&tnow, NULL);
	timersub(&tnow, &state->vs_tlast, &delta);
	if (delta.tv_sec < 1)
		return (0);
	state->vs_tlast.tv_sec = tnow.tv_sec;
	state->vs_tlast.tv_usec = tnow.tv_usec;

	op = (rte->ri_flags & RI_VALID) ? CMD_HEARTBEAT : CMD_ROUTE_QUERY;
	uvxcmd_fill(txbuf, state->vs_ctrl_mac, state->vs_prov_mac, op, 0, 0);

	/* update all datapath threads with a copy of the latest state */
	dp_count = state->vs_datapath_count;
	if (dp_count) {
		newstate = new vxstate_t(*state);
		oldstate = state->vs_dp_states[0]->vsd_state;
		for (int i = 0; i < dp_count; i++)
			state->vs_dp_states[i]->vsd_state = newstate;
		/* XXX --- clunky LOLEBR --- sleep for 50ms before freeing */
		if (__predict_true(debug == 0)) {
			usleep(50000);
			if (__predict_false(oldstate != state))
				delete oldstate;
		}
	}
	return (count);
}


/*
 * generic handler for sockopt functions
 */
static int
ctl_handler(struct sockopt *sopt, struct ip_fw_chain *chain)
{
	int error = EINVAL;

	ND("called, level %d", sopt->sopt_level);
	if (sopt->sopt_level != IPPROTO_IP)
		return (EINVAL);
	switch (sopt->sopt_name) {
	default:
		D("command not recognised %d", sopt->sopt_name);
		break;
	case IP_FW3: // XXX untested
	case IP_FW_ADD: /* ADD actually returns the body... */
	case IP_FW_GET:
	case IP_FW_DEL:
	case IP_FW_TABLE_GETSIZE:
	case IP_FW_TABLE_LIST:
	case IP_FW_NAT_GET_CONFIG:
	case IP_FW_NAT_GET_LOG:
	case IP_FW_FLUSH:
	case IP_FW_ZERO:
	case IP_FW_RESETLOG:
	case IP_FW_TABLE_ADD:
	case IP_FW_TABLE_DEL:
	case IP_FW_TABLE_FLUSH:
	case IP_FW_NAT_CFG:
	case IP_FW_NAT_DEL:
		if (ip_fw_ctl_ptr != NULL)
			error = ip_fw_ctl_ptr(sopt, chain);
		else {
			D("ipfw not enabled");
			error = ENOPROTOOPT;
		}
		break;
#ifdef __unused__
	case IP_DUMMYNET_GET:
	case IP_DUMMYNET_CONFIGURE:
	case IP_DUMMYNET_DEL:
	case IP_DUMMYNET_FLUSH:
	case IP_DUMMYNET3:
		if (ip_dn_ctl_ptr != NULL)
			error = ip_dn_ctl_ptr(sopt);
		else
			error = ENOPROTOOPT;
		break ;
#endif
	}
	ND("returning error %d", error);
	return error;
}

int
cmd_dispatch_ipfw(struct ipfw_wire_hdr *ipfw, char *txdata, vxstate_t *state)
{
	struct thread dummy;
	intf_info_map_t &intftbl = state->vs_intf_table;
	socklen_t optlen = 0;
	uint64_t mac;
	struct sockopt sopt;
	void *optval  = (void *)(uintptr_t)(ipfw + 1);
	struct ip_fw_chain *chain;

	mac = ((uint64_t)ipfw->mac) & 0xFFFFFFFFFFFF;
	auto it = intftbl.find(mac);
	if (it == intftbl.end()) {
		ipfw->level = htonl(ENOENT);
		ipfw->optlen = htonl(0);
		goto done;
	}
	chain = it->second->ii_chain;
	sopt.sopt_dir = (enum sopt_dir)ntohl(ipfw->dir);
	sopt.sopt_level = ntohl(ipfw->level);
	sopt.sopt_val = optval;
	sopt.sopt_name = ntohl(ipfw->optname);
	sopt.sopt_valsize = ntohl(ipfw->optlen);
	sopt.sopt_td = &dummy;

	ipfw->level = htonl(ctl_handler(&sopt, chain));
	ipfw->optlen = htonl(0);
	ipfw->dir = htonl(sopt.sopt_dir);
	if (sopt.sopt_dir == SOPT_GET) {
		ipfw->optlen = htonl(sopt.sopt_valsize);
		optlen = sopt.sopt_valsize;
	}
  done:
	memcpy(txdata, ipfw, sizeof(*ipfw) + optlen);
	return (sizeof(*ipfw) + optlen);
}

#if 0
int
cmd_dispatch_ip(char *rxbuf, char *txbuf, path_state_t *ps, vxstate_t *state)
{
	struct ip *ip = (struct ip *)(uintptr_t)(rxbuf + ETHER_HDR_LEN);
	struct udphdr *uh = (struct udphdr *)(uintptr_t)((caddr_t)ip + (ip->ip_hl << 2));
	struct dhcp *bp = (struct dhcp *)(uintptr_t)(uh + 1);
	struct ipfw_wire_hdr *ipfw = (struct ipfw_wire_hdr *)(uintptr_t)(uh + 1);
	uint16_t dport;
	/* validate ip header */

	/* validate the UDP header */
	dport = ntohs(uh->uh_dport);

	/* we're only supporting a BOOTP response for the moment */
	if (ps->ps_rx_len < sizeof(*bp) + BP_MSG_OVERHEAD)
		return 0;

	if (dport == IPPORT_BOOTPS && cmd_dispatch_bp(bp, state)) {
		char *txbuf_;
		path_state_t psgrat;

		bzero(&psgrat, sizeof(path_state_t));
		if ((txbuf_ = get_txbuf(&psgrat, state->vs_nm_egress)) == NULL)
			return (0);
		/*
		 * we have our address -- now we want to send out a
		 * gratuitous ARP for the switch
		 */
		data_send_arp_phys(rxbuf, txbuf_, &psgrat, state, 1);
		txring_next(&psgrat, 60);
		/* XXX proactively resolve the MAC address for the gateway */
		/* ... */
	} else if (dport == IPPORT_IPFWPS) {
		return cmd_dispatch_ipfw(ipfw, txbuf, ps, state);
	}
	return (0);
}
#endif

static uint32_t
genmask(int prefixlen)
{
		uint64_t mask = (1UL << prefixlen)-1;
		mask <<= (32-prefixlen);
		return static_cast<uint32_t>(mask);
}

int
cmd_dispatch_config(char *rxbuf, char *txbuf, path_state_t *ps, void *arg)
{
	vxstate_t *state = (vxstate_t *)arg;
	struct uvxcmd_header *uh = (struct uvxcmd_header *)(rxbuf + sizeof(struct ether_header));
	caddr_t rxdata = (caddr_t)(uh + 1);
	caddr_t txdata = txbuf + sizeof(struct ether_header) + sizeof(struct uvxcmd_header);	
	uint64_t mac;
	uint16_t size, rc, op;

	size = rc = op = 0;
	
	if (uh->uh_magic != UVXMAGIC)
		return (0);

	switch (uh->uh_op) {
		case CMD_ARP_REQUEST: {
			struct arp_request *arq = (struct arp_request *)rxdata;
			struct arp_reply *are = (struct arp_reply *)txdata;
			l2tbl_t &l2tbl = state->vs_l2_phys;
			auto it = l2tbl.l2t_v4.find(arq->ar_pa);
			op = CMD_ARP_REPLY;
			
			if (it != l2tbl.l2t_v4.end()) {
				u64tomac(it->second, are->ar_ha);
				are->ar_pa = arq->ar_pa;
				size = sizeof(*are);
			} else {
				rc = ENOENT;
			}
		}
			break;
		case CMD_ARP_REPLY: {
			struct arp_reply *are = (struct arp_reply *)rxdata;
			l2tbl_t &l2tbl = state->vs_l2_phys;
			mac = mactou64(are->ar_ha);
			if (mac)
				l2tbl.l2t_v4.insert(pair<uint32_t, uint64_t>(are->ar_pa, mac));
			else
				l2tbl.l2t_v4.erase(are->ar_pa);
		}
			break;
		case CMD_FTE_REQUEST: {
			ftablemap_t &ftablemap = state->vs_ftables;
			struct fte_request *frq = (struct fte_request *)rxdata;
			struct fte_reply *fre = (struct fte_reply *)txdata;
			mac = mactou64(frq->fr_ha);
			op = CMD_FTE_REPLY;
			auto ftable_it = ftablemap.find(frq->fr_vxlanid);
			if (ftable_it == ftablemap.end()) {
				rc = ENOENT;
				break;
			}
			auto it = ftable_it->second.find(mac);
			if (it != ftable_it->second.end() && it->second.vfe_v6 == 0) {
				fre->fr_pa = it->second.vfe_raddr.in4.s_addr;
			} else
				fre->fr_pa = 0;
			u64tomac(mac, fre->fr_ha);
			fre->fr_vxlanid = frq->fr_vxlanid;
			size = sizeof(*fre);
		}
			break;
		case CMD_FTE_REPLY: {
			ftablemap_t &ftablemap = state->vs_ftables;
			struct fte_reply *fre = (struct fte_reply *)rxdata;
			auto ftable_it = ftablemap.find(fre->fr_vxlanid);

			mac = mactou64(fre->fr_ha);
			if (ftable_it == ftablemap.end()) {
				rc = ENOENT;
				break;
			}
			if (fre->fr_pa == 0)
				ftable_it->second.erase(mac);
			else {
				vfe_t vfe;
				vfe.vfe_raddr.in4.s_addr = fre->fr_pa;
				ftable_it->second.insert(fwdent(mac, vfe));
				/* XXX proactively resolve the MAC address for tpa */
			}
		}
			break;
		case CMD_VM_INTF_REQUEST: {
			intf_info_map_t &intftbl = state->vs_intf_table;
			struct vm_intf_request *virq = (struct vm_intf_request *)rxdata;
			struct vm_intf_reply *vire = (struct vm_intf_reply *)txdata;
			mac = mactou64(virq->vir_ha);
			auto it = intftbl.find(mac);
			if (it != intftbl.end()) {
				intf_info_t *ii = it->second;
				u64tomac(mac, vire->vir_ha);
				op = CMD_VM_INTF_REPLY;
				vire->vir_vlanid = ii->ii_ent.fields.vlanid;
				vire->vir_vxlanid = ii->ii_ent.fields.vxlanid;
				vire->vir_flags =  ii->ii_ent.fields.flags;
				size = sizeof(*vire);
			} else
				rc = ENOENT;
		}
			break;
		case CMD_VM_INTF_REPLY: {
			ftablemap_t &ftablemap = state->vs_ftables;
			intf_info_map_t &intftbl = state->vs_intf_table;
			struct vm_intf_reply *vire = (struct vm_intf_reply *)rxdata;
			mac = mactou64(vire->vir_ha);

			if (vire->vir_vxlanid == 0) {
				uint32_t vxlanid;
				auto it = intftbl.find(mac);

				if (it != intftbl.end()) {
					vxlanid = it->second->ii_ent.fields.vxlanid;
					auto it_ftable = ftablemap.find(vxlanid);
					if (it_ftable != ftablemap.end() &&
						it_ftable->second.size() == 0)
						ftablemap.erase(vxlanid);

					delete it->second;
					intftbl.erase(mac);
				}
			} else {
				intf_info_t *ii;
				bool insert = false;
				auto it = intftbl.find(mac);

				if (it != intftbl.end()) {
					ii = it->second;
				} else {
					ii = new intf_info();
					insert = true;
				}
				ii->ii_ent.fields.vxlanid = vire->vir_vxlanid;
				ii->ii_ent.fields.vlanid = vire->vir_vlanid;
				ii->ii_ent.fields.flags = vire->vir_flags;
				if (insert)
					intftbl.insert(pair<uint64_t, intf_info_t*>(mac, ii));

				auto it_ftable = ftablemap.find(vire->vir_vxlanid);
				if (it_ftable == ftablemap.end()) {
					ftable_t ftable;
					ftablemap.insert(pair<uint32_t, ftable_t>(vire->vir_vxlanid, ftable));
				}
			}
		}
			break;
		case CMD_DTLS_SERVCONF: {
			struct dtls_configure_server *dcs = (struct dtls_configure_server *)rxdata;
			int dp_count = state->vs_datapath_count;
			vxstate_dp_t *dp_state;
			int i, j;

			for (i = 0; i < dp_count; i++) {
				dp_state = state->vs_dp_states[i];
				dp_state->vsd_channel = dtls_channel_alloc(
					dp_state->vsd_session_mgr,
					dcs->dcs_psk,
					dp_state->vsd_policy,
					dp_state->vsd_rng);

				if (dp_state->vsd_channel == NULL)
					break;
			}
			if (i != dp_count) {
				for (j = 0; j < i; j++)
					state->vs_dp_states[j]->vsd_channel = NULL;
				rc = ENOMEM;
			}
		}
			break;
		case CMD_DTLS_CLICONF:
			rc = ENOSYS;
			break;
		case CMD_DTLS_QUERY:
			rc = ENOSYS;
			break;
		case CMD_ROUTE_CONFIGURE: {
			rte_t *rte = &state->vs_dflt_rte;
			struct route_configure *rc = (struct route_configure *)rxdata;

			rte->ri_prefixlen = rc->rc_prefixlen;
			rte->ri_mask.in4.s_addr = genmask(rc->rc_prefixlen);
			rte->ri_laddr.in4.s_addr = rc->rc_lpa;
			rte->ri_raddr.in4.s_addr = rc->rc_rpa;
			rte->ri_flags = RI_VALID;
			DBG("route installed\n");
		}
			break;
		case CMD_ROUTE_QUERY:
			break;
		case CMD_IPFW: {
			struct ipfw_wire_hdr *ipfw = (struct ipfw_wire_hdr *)rxdata;
			size = cmd_dispatch_ipfw(ipfw, txdata, state);
			rc = ntohl(ipfw->level);
			op = CMD_IPFW;
		}
			break;
		default:
			rc = ENOSYS;
			op = 0;
			break;
	}
	uvxcmd_fill(txbuf, state->vs_ctrl_mac, state->vs_prov_mac, op, rc, uh->uh_seqno);
	*ps->ps_tx_len = sizeof(struct ether_header) + sizeof(struct uvxcmd_header) + size;
	return (1);
}
