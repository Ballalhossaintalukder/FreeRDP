/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * RDP Server Peer
 *
 * Copyright 2011 Vic Lee
 * Copyright 2014 DI (FH) Martin Haimberger <martin.haimberger@thincast.com>
 * Copyright 2023 Armin Novak <anovak@thincast.com>
 * Copyright 2023 Thincast Technologies GmbH
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <freerdp/config.h>

#include "settings.h"

#include <winpr/assert.h>
#include <winpr/cast.h>
#include <winpr/crt.h>
#include <winpr/winsock.h>

#include "info.h"
#include "display.h"

#include <freerdp/log.h>
#include <freerdp/streamdump.h>
#include <freerdp/redirection.h>
#include <freerdp/crypto/certificate.h>

#include "rdp.h"
#include "peer.h"
#include "multitransport.h"

#define TAG FREERDP_TAG("core.peer")

static state_run_t peer_recv_pdu(freerdp_peer* client, wStream* s);

static HANDLE freerdp_peer_virtual_channel_open(freerdp_peer* client, const char* name,
                                                UINT32 flags)
{
	UINT32 index = 0;
	BOOL joined = FALSE;
	rdpMcsChannel* mcsChannel = NULL;
	rdpPeerChannel* peerChannel = NULL;
	rdpMcs* mcs = NULL;

	WINPR_ASSERT(client);
	WINPR_ASSERT(client->context);
	WINPR_ASSERT(client->context->rdp);
	WINPR_ASSERT(name);
	mcs = client->context->rdp->mcs;
	WINPR_ASSERT(mcs);

	if (flags & WTS_CHANNEL_OPTION_DYNAMIC)
		return NULL; /* not yet supported */

	const size_t length = strnlen(name, 9);

	if (length > 8)
		return NULL; /* SVC maximum name length is 8 */

	for (; index < mcs->channelCount; index++)
	{
		mcsChannel = &(mcs->channels[index]);

		if (!mcsChannel->joined)
			continue;

		if (_strnicmp(name, mcsChannel->Name, length) == 0)
		{
			joined = TRUE;
			break;
		}
	}

	if (!joined)
		return NULL; /* channel is not joined */

	peerChannel = (rdpPeerChannel*)mcsChannel->handle;

	if (peerChannel)
	{
		/* channel is already open */
		return (HANDLE)peerChannel;
	}

	WINPR_ASSERT(index <= UINT16_MAX);
	peerChannel =
	    server_channel_common_new(client, (UINT16)index, mcsChannel->ChannelId, 128, NULL, name);

	if (peerChannel)
	{
		peerChannel->channelFlags = flags;
		peerChannel->mcsChannel = mcsChannel;
		mcsChannel->handle = (void*)peerChannel;
	}

	return (HANDLE)peerChannel;
}

static BOOL freerdp_peer_virtual_channel_close(WINPR_ATTR_UNUSED freerdp_peer* client,
                                               HANDLE hChannel)
{
	rdpMcsChannel* mcsChannel = NULL;
	rdpPeerChannel* peerChannel = NULL;

	WINPR_ASSERT(client);

	if (!hChannel)
		return FALSE;

	peerChannel = (rdpPeerChannel*)hChannel;
	mcsChannel = peerChannel->mcsChannel;
	WINPR_ASSERT(mcsChannel);
	mcsChannel->handle = NULL;
	server_channel_common_free(peerChannel);
	return TRUE;
}

static int freerdp_peer_virtual_channel_write(freerdp_peer* client, HANDLE hChannel,
                                              const BYTE* buffer, UINT32 length)
{
	wStream* s = NULL;
	UINT32 flags = 0;
	UINT32 chunkSize = 0;
	UINT32 maxChunkSize = 0;
	UINT32 totalLength = 0;
	rdpPeerChannel* peerChannel = NULL;
	rdpMcsChannel* mcsChannel = NULL;
	rdpRdp* rdp = NULL;

	WINPR_ASSERT(client);
	WINPR_ASSERT(client->context);

	rdp = client->context->rdp;
	WINPR_ASSERT(rdp);
	WINPR_ASSERT(rdp->settings);

	if (!hChannel)
		return -1;

	peerChannel = (rdpPeerChannel*)hChannel;
	mcsChannel = peerChannel->mcsChannel;
	WINPR_ASSERT(peerChannel);
	WINPR_ASSERT(mcsChannel);
	if (peerChannel->channelFlags & WTS_CHANNEL_OPTION_DYNAMIC)
		return -1; /* not yet supported */

	maxChunkSize = rdp->settings->VCChunkSize;
	totalLength = length;
	flags = CHANNEL_FLAG_FIRST;

	while (length > 0)
	{
		UINT16 sec_flags = 0;
		s = rdp_send_stream_init(rdp, &sec_flags);

		if (!s)
			return -1;

		if (length > maxChunkSize)
		{
			chunkSize = rdp->settings->VCChunkSize;
		}
		else
		{
			chunkSize = length;
			flags |= CHANNEL_FLAG_LAST;
		}

		if (mcsChannel->options & CHANNEL_OPTION_SHOW_PROTOCOL)
			flags |= CHANNEL_FLAG_SHOW_PROTOCOL;

		Stream_Write_UINT32(s, totalLength);
		Stream_Write_UINT32(s, flags);

		if (!Stream_EnsureRemainingCapacity(s, chunkSize))
		{
			Stream_Release(s);
			return -1;
		}

		Stream_Write(s, buffer, chunkSize);

		WINPR_ASSERT(peerChannel->channelId <= UINT16_MAX);
		if (!rdp_send(rdp, s, (UINT16)peerChannel->channelId, sec_flags))
			return -1;

		buffer += chunkSize;
		length -= chunkSize;
		flags = 0;
	}

	return 1;
}

static void* freerdp_peer_virtual_channel_get_data(WINPR_ATTR_UNUSED freerdp_peer* client,
                                                   HANDLE hChannel)
{
	rdpPeerChannel* peerChannel = (rdpPeerChannel*)hChannel;

	WINPR_ASSERT(client);
	if (!hChannel)
		return NULL;

	return peerChannel->extra;
}

static int freerdp_peer_virtual_channel_set_data(WINPR_ATTR_UNUSED freerdp_peer* client,
                                                 HANDLE hChannel, void* data)
{
	rdpPeerChannel* peerChannel = (rdpPeerChannel*)hChannel;

	WINPR_ASSERT(client);
	if (!hChannel)
		return -1;

	peerChannel->extra = data;
	return 1;
}

static BOOL freerdp_peer_set_state(freerdp_peer* client, CONNECTION_STATE state)
{
	WINPR_ASSERT(client);
	WINPR_ASSERT(client->context);
	return rdp_server_transition_to_state(client->context->rdp, state);
}

static BOOL freerdp_peer_initialize(freerdp_peer* client)
{
	rdpRdp* rdp = NULL;
	rdpSettings* settings = NULL;

	WINPR_ASSERT(client);
	WINPR_ASSERT(client->context);

	rdp = client->context->rdp;
	WINPR_ASSERT(rdp);

	settings = rdp->settings;
	WINPR_ASSERT(settings);

	settings->ServerMode = TRUE;
	settings->FrameAcknowledge = 0;
	settings->LocalConnection = client->local;

	const rdpCertificate* cert =
	    freerdp_settings_get_pointer(settings, FreeRDP_RdpServerCertificate);
	if (!cert)
	{
		WLog_ERR(TAG, "Missing server certificate, can not continue.");
		return FALSE;
	}

	if (freerdp_settings_get_bool(settings, FreeRDP_RdpSecurity))
	{

		if (!freerdp_certificate_is_rdp_security_compatible(cert))
		{
			if (!freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, FALSE))
				return FALSE;
			if (!freerdp_settings_set_bool(settings, FreeRDP_UseRdpSecurityLayer, FALSE))
				return FALSE;
		}
	}

	nego_set_RCG_supported(rdp->nego, settings->RemoteCredentialGuard);
	nego_set_restricted_admin_mode_supported(rdp->nego, settings->RestrictedAdminModeSupported);

	if (!rdp_server_transition_to_state(rdp, CONNECTION_STATE_INITIAL))
		return FALSE;

	return TRUE;
}

#if defined(WITH_FREERDP_DEPRECATED)
static BOOL freerdp_peer_get_fds(freerdp_peer* client, void** rfds, int* rcount)
{
	rdpTransport* transport = NULL;
	WINPR_ASSERT(client);
	WINPR_ASSERT(client->context);
	WINPR_ASSERT(client->context->rdp);

	transport = client->context->rdp->transport;
	WINPR_ASSERT(transport);
	transport_get_fds(transport, rfds, rcount);
	return TRUE;
}
#endif

static HANDLE freerdp_peer_get_event_handle(freerdp_peer* client)
{
	HANDLE hEvent = NULL;
	rdpTransport* transport = NULL;
	WINPR_ASSERT(client);
	WINPR_ASSERT(client->context);
	WINPR_ASSERT(client->context->rdp);

	transport = client->context->rdp->transport;
	hEvent = transport_get_front_bio(transport);
	return hEvent;
}

static DWORD freerdp_peer_get_event_handles(freerdp_peer* client, HANDLE* events, DWORD count)
{
	WINPR_ASSERT(client);
	WINPR_ASSERT(client->context);
	WINPR_ASSERT(client->context->rdp);
	return transport_get_event_handles(client->context->rdp->transport, events, count);
}

static BOOL freerdp_peer_check_fds(freerdp_peer* peer)
{
	int status = 0;
	rdpRdp* rdp = NULL;

	WINPR_ASSERT(peer);
	WINPR_ASSERT(peer->context);

	rdp = peer->context->rdp;
	status = rdp_check_fds(rdp);

	if (status < 0)
		return FALSE;

	return TRUE;
}

static state_run_t peer_recv_data_pdu(freerdp_peer* client, wStream* s,
                                      WINPR_ATTR_UNUSED UINT16 totalLength)
{
	BYTE type = 0;
	UINT16 length = 0;
	UINT32 share_id = 0;
	BYTE compressed_type = 0;
	UINT16 compressed_len = 0;
	rdpUpdate* update = NULL;

	WINPR_ASSERT(s);
	WINPR_ASSERT(client);
	WINPR_ASSERT(client->context);
	rdpRdp* rdp = client->context->rdp;
	WINPR_ASSERT(rdp);
	WINPR_ASSERT(rdp->mcs);

	update = client->context->update;
	WINPR_ASSERT(update);

	if (!rdp_read_share_data_header(rdp, s, &length, &type, &share_id, &compressed_type,
	                                &compressed_len))
		return STATE_RUN_FAILED;

#ifdef WITH_DEBUG_RDP
	WLog_Print(rdp->log, WLOG_DEBUG, "recv %s Data PDU (0x%02" PRIX8 "), length: %" PRIu16 "",
	           data_pdu_type_to_string(type), type, length);
#endif

	switch (type)
	{
		case DATA_PDU_TYPE_SYNCHRONIZE:
			if (!rdp_recv_client_synchronize_pdu(rdp, s))
				return STATE_RUN_FAILED;

			break;

		case DATA_PDU_TYPE_CONTROL:
			if (!rdp_server_accept_client_control_pdu(rdp, s))
				return STATE_RUN_FAILED;

			break;

		case DATA_PDU_TYPE_INPUT:
			if (!input_recv(rdp->input, s))
				return STATE_RUN_FAILED;

			break;

		case DATA_PDU_TYPE_BITMAP_CACHE_PERSISTENT_LIST:
			if (!rdp_server_accept_client_persistent_key_list_pdu(rdp, s))
				return STATE_RUN_FAILED;
			break;

		case DATA_PDU_TYPE_FONT_LIST:
			if (!rdp_server_accept_client_font_list_pdu(rdp, s))
				return STATE_RUN_FAILED;

			return STATE_RUN_CONTINUE; // State changed, trigger rerun

		case DATA_PDU_TYPE_SHUTDOWN_REQUEST:
			mcs_send_disconnect_provider_ultimatum(rdp->mcs,
			                                       Disconnect_Ultimatum_provider_initiated);
			WLog_WARN(TAG, "disconnect provider ultimatum sent to peer, closing connection");
			return STATE_RUN_QUIT_SESSION;

		case DATA_PDU_TYPE_FRAME_ACKNOWLEDGE:
			if (!Stream_CheckAndLogRequiredLength(TAG, s, 4))
				return STATE_RUN_FAILED;

			Stream_Read_UINT32(s, client->ack_frame_id);
			IFCALL(update->SurfaceFrameAcknowledge, update->context, client->ack_frame_id);
			break;

		case DATA_PDU_TYPE_REFRESH_RECT:
			if (!update_read_refresh_rect(update, s))
				return STATE_RUN_FAILED;

			break;

		case DATA_PDU_TYPE_SUPPRESS_OUTPUT:
			if (!update_read_suppress_output(update, s))
				return STATE_RUN_FAILED;

			break;

		default:
			WLog_ERR(TAG, "Data PDU type %" PRIu8 "", type);
			break;
	}

	return STATE_RUN_SUCCESS;
}

static state_run_t peer_recv_tpkt_pdu(freerdp_peer* client, wStream* s)
{
	state_run_t rc = STATE_RUN_SUCCESS;
	UINT16 length = 0;
	UINT16 pduType = 0;
	UINT16 pduSource = 0;
	UINT16 channelId = 0;
	UINT16 securityFlags = 0;

	WINPR_ASSERT(s);
	WINPR_ASSERT(client);
	WINPR_ASSERT(client->context);

	rdpRdp* rdp = client->context->rdp;
	WINPR_ASSERT(rdp);
	WINPR_ASSERT(rdp->mcs);

	rdpSettings* settings = client->context->settings;
	WINPR_ASSERT(settings);

	if (!rdp_read_header(rdp, s, &length, &channelId))
		return STATE_RUN_FAILED;

	rdp->inPackets++;
	if (freerdp_shall_disconnect_context(rdp->context))
		return STATE_RUN_SUCCESS;

	if (rdp_get_state(rdp) <= CONNECTION_STATE_LICENSING)
	{
		return rdp_handle_message_channel(rdp, s, channelId, length);
	}

	if (!rdp_handle_optional_rdp_decryption(rdp, s, &length, &securityFlags))
		return STATE_RUN_FAILED;

	if (channelId == MCS_GLOBAL_CHANNEL_ID)
	{
		char buffer[256] = { 0 };
		UINT16 pduLength = 0;
		UINT16 remain = 0;
		if (!rdp_read_share_control_header(rdp, s, &pduLength, &remain, &pduType, &pduSource))
			return STATE_RUN_FAILED;

		settings->PduSource = pduSource;

		WLog_DBG(TAG, "Received %s", pdu_type_to_str(pduType, buffer, sizeof(buffer)));
		switch (pduType)
		{
			case PDU_TYPE_DATA:
				rc = peer_recv_data_pdu(client, s, pduLength);
				break;

			case PDU_TYPE_CONFIRM_ACTIVE:
				if (!rdp_server_accept_confirm_active(rdp, s, pduLength))
					return STATE_RUN_FAILED;

				break;

			case PDU_TYPE_FLOW_RESPONSE:
			case PDU_TYPE_FLOW_STOP:
			case PDU_TYPE_FLOW_TEST:
				if (!Stream_SafeSeek(s, remain))
				{
					WLog_WARN(TAG, "Short PDU, need %" PRIuz " bytes, got %" PRIuz, remain,
					          Stream_GetRemainingLength(s));
					return STATE_RUN_FAILED;
				}
				break;

			default:
				WLog_ERR(TAG, "Client sent unknown pduType %" PRIu16 "", pduType);
				return STATE_RUN_FAILED;
		}
	}
	else if ((rdp->mcs->messageChannelId > 0) && (channelId == rdp->mcs->messageChannelId))
	{
		if (!settings->UseRdpSecurityLayer)
		{
			if (!rdp_read_security_header(rdp, s, &securityFlags, NULL))
				return STATE_RUN_FAILED;
		}

		return rdp_recv_message_channel_pdu(rdp, s, securityFlags);
	}
	else
	{
		if (!freerdp_channel_peer_process(client, s, channelId))
			return STATE_RUN_FAILED;
	}
	if (!tpkt_ensure_stream_consumed(rdp->log, s, length))
		return STATE_RUN_FAILED;

	return rc;
}

static state_run_t peer_recv_handle_auto_detect(freerdp_peer* client, wStream* s)
{
	state_run_t ret = STATE_RUN_FAILED;
	rdpRdp* rdp = NULL;

	WINPR_ASSERT(client);
	WINPR_ASSERT(s);
	WINPR_ASSERT(client->context);

	rdp = client->context->rdp;
	WINPR_ASSERT(rdp);

	const rdpSettings* settings = client->context->settings;
	WINPR_ASSERT(settings);

	if (freerdp_settings_get_bool(settings, FreeRDP_NetworkAutoDetect))
	{
		switch (rdp_get_state(rdp))
		{
			case CONNECTION_STATE_CONNECT_TIME_AUTO_DETECT_REQUEST:
				autodetect_on_connect_time_auto_detect_begin(rdp->autodetect);
				switch (autodetect_get_state(rdp->autodetect))
				{
					case FREERDP_AUTODETECT_STATE_REQUEST:
						ret = STATE_RUN_SUCCESS;
						if (!rdp_server_transition_to_state(
						        rdp, CONNECTION_STATE_CONNECT_TIME_AUTO_DETECT_RESPONSE))
							return STATE_RUN_FAILED;
						break;
					case FREERDP_AUTODETECT_STATE_COMPLETE:
						ret = STATE_RUN_CONTINUE; /* Rerun in next state */
						if (!rdp_server_transition_to_state(rdp, CONNECTION_STATE_LICENSING))
							return STATE_RUN_FAILED;
						break;
					default:
						break;
				}
				break;
			case CONNECTION_STATE_CONNECT_TIME_AUTO_DETECT_RESPONSE:
				ret = peer_recv_pdu(client, s);
				if (state_run_success(ret))
				{
					autodetect_on_connect_time_auto_detect_progress(rdp->autodetect);
					switch (autodetect_get_state(rdp->autodetect))
					{
						case FREERDP_AUTODETECT_STATE_REQUEST:
							ret = STATE_RUN_SUCCESS;
							break;
						case FREERDP_AUTODETECT_STATE_COMPLETE:
							ret = STATE_RUN_CONTINUE; /* Rerun in next state */
							if (!rdp_server_transition_to_state(rdp, CONNECTION_STATE_LICENSING))
								return STATE_RUN_FAILED;
							break;
						default:
							break;
					}
				}
				break;
			default:
				WINPR_ASSERT(FALSE);
				break;
		}
	}
	else
	{
		if (!rdp_server_transition_to_state(rdp, CONNECTION_STATE_LICENSING))
			return STATE_RUN_FAILED;

		ret = STATE_RUN_CONTINUE; /* Rerun in next state */
	}

	return ret;
}

static state_run_t peer_recv_handle_licensing(freerdp_peer* client, wStream* s)
{
	state_run_t ret = STATE_RUN_FAILED;
	rdpRdp* rdp = NULL;
	rdpSettings* settings = NULL;

	WINPR_ASSERT(client);
	WINPR_ASSERT(s);
	WINPR_ASSERT(client->context);

	rdp = client->context->rdp;
	WINPR_ASSERT(rdp);

	settings = rdp->settings;
	WINPR_ASSERT(settings);

	switch (license_get_state(rdp->license))
	{
		case LICENSE_STATE_INITIAL:
		{
			const BOOL required =
			    freerdp_settings_get_bool(settings, FreeRDP_ServerLicenseRequired);

			if (required)
			{
				if (!license_server_configure(rdp->license))
					ret = STATE_RUN_FAILED;
				else if (!license_server_send_request(rdp->license))
					ret = STATE_RUN_FAILED;
				else
					ret = STATE_RUN_SUCCESS;
			}
			else
			{
				if (license_send_valid_client_error_packet(rdp))
					ret = STATE_RUN_CONTINUE; /* Rerun in next state, might be capabilities */
			}
		}
		break;
		case LICENSE_STATE_COMPLETED:
			ret = STATE_RUN_CONTINUE; /* Licensing completed, continue in next state */
			break;
		case LICENSE_STATE_ABORTED:
			ret = STATE_RUN_FAILED;
			break;
		default:
			ret = peer_recv_pdu(client, s);
			break;
	}

	return ret;
}

static state_run_t peer_recv_fastpath_pdu(freerdp_peer* client, wStream* s)
{
	rdpRdp* rdp = NULL;
	UINT16 length = 0;
	BOOL rc = 0;
	rdpFastPath* fastpath = NULL;

	WINPR_ASSERT(s);
	WINPR_ASSERT(client);
	WINPR_ASSERT(client->context);

	rdp = client->context->rdp;
	WINPR_ASSERT(rdp);

	fastpath = rdp->fastpath;
	WINPR_ASSERT(fastpath);

	rc = fastpath_read_header_rdp(fastpath, s, &length);

	if (!rc || (length == 0))
	{
		WLog_ERR(TAG, "incorrect FastPath PDU header length %" PRIu16 "", length);
		return STATE_RUN_FAILED;
	}
	if (!Stream_CheckAndLogRequiredLength(TAG, s, length))
		return STATE_RUN_FAILED;

	if (!fastpath_decrypt(fastpath, s, &length))
		return STATE_RUN_FAILED;

	rdp->inPackets++;

	return fastpath_recv_inputs(fastpath, s);
}

state_run_t peer_recv_pdu(freerdp_peer* client, wStream* s)
{
	int rc = tpkt_verify_header(s);

	if (rc > 0)
		return peer_recv_tpkt_pdu(client, s);
	else if (rc == 0)
		return peer_recv_fastpath_pdu(client, s);
	else
		return STATE_RUN_FAILED;
}

static state_run_t peer_unexpected_client_message(rdpRdp* rdp, UINT32 flag)
{
	char buffer[1024] = { 0 };
	WLog_WARN(TAG, "Unexpected client message in state %s, missing flag %s",
	          rdp_get_state_string(rdp), rdp_finalize_flags_to_str(flag, buffer, sizeof(buffer)));
	return STATE_RUN_SUCCESS; /* we ignore this as per spec input PDU are already allowed */
}

state_run_t rdp_peer_handle_state_demand_active(freerdp_peer* client)
{
	state_run_t ret = STATE_RUN_FAILED;

	WINPR_ASSERT(client);
	WINPR_ASSERT(client->context);

	rdpRdp* rdp = client->context->rdp;
	WINPR_ASSERT(rdp);

	if (client->Capabilities && !client->Capabilities(client))
	{
		WLog_ERR(TAG, "[%s] freerdp_peer::Capabilities() callback failed",
		         rdp_get_state_string(rdp));
	}
	else if (!rdp_send_demand_active(rdp))
	{
		WLog_ERR(TAG, "[%s] rdp_send_demand_active() fail", rdp_get_state_string(rdp));
	}
	else
	{
		if (!rdp_server_transition_to_state(rdp,
		                                    CONNECTION_STATE_CAPABILITIES_EXCHANGE_MONITOR_LAYOUT))
			return STATE_RUN_FAILED;
		ret = STATE_RUN_CONTINUE;
	}
	return ret;
}

/** \brief Handle server peer state ACTIVE:
 *  On initial run (not connected, not activated) do not read data
 *
 *  \return -1 in case of an error, 0 if no data needs to be processed, 1 to let
 *  the state machine run again and 2 if peer_recv_pdu must be called.
 */
static state_run_t rdp_peer_handle_state_active(freerdp_peer* client)
{
	state_run_t ret = STATE_RUN_FAILED;

	WINPR_ASSERT(client);
	WINPR_ASSERT(client->context);

	if (!client->connected)
	{
		/**
		 * PostConnect should only be called once and should not
		 * be called after a reactivation sequence.
		 */
		IFCALLRET(client->PostConnect, client->connected, client);
	}
	if (!client->connected)
	{
		WLog_ERR(TAG, "PostConnect for peer %p failed", client);
		ret = STATE_RUN_FAILED;
	}
	else if (!client->activated)
	{
		BOOL activated = TRUE;

		/*  Set client->activated TRUE before calling the Activate callback.
		 *  the Activate callback might reset the client->activated flag even if it returns success
		 * (e.g. deactivate/reactivate sequence) */
		client->activated = TRUE;
		IFCALLRET(client->Activate, activated, client);

		if (!activated)
		{
			WLog_ERR(TAG, "Activate for peer %p failed", client);
			ret = STATE_RUN_FAILED;
		}
		else
			ret = STATE_RUN_SUCCESS;
	}
	else
		ret = STATE_RUN_ACTIVE;
	return ret;
}

static state_run_t peer_recv_callback_internal(WINPR_ATTR_UNUSED rdpTransport* transport,
                                               wStream* s, void* extra)
{
	UINT32 SelectedProtocol = 0;
	freerdp_peer* client = (freerdp_peer*)extra;
	rdpRdp* rdp = NULL;
	state_run_t ret = STATE_RUN_FAILED;
	rdpSettings* settings = NULL;

	WINPR_ASSERT(transport);
	WINPR_ASSERT(client);
	WINPR_ASSERT(client->context);

	rdp = client->context->rdp;
	WINPR_ASSERT(rdp);

	settings = client->context->settings;
	WINPR_ASSERT(settings);

	IFCALL(client->ReachedState, client, rdp_get_state(rdp));
	switch (rdp_get_state(rdp))
	{
		case CONNECTION_STATE_INITIAL:
			if (!freerdp_settings_enforce_consistency(settings))
				ret = STATE_RUN_FAILED;
			else if (rdp_server_transition_to_state(rdp, CONNECTION_STATE_NEGO))
				ret = STATE_RUN_CONTINUE;
			break;

		case CONNECTION_STATE_NEGO:
			if (!rdp_server_accept_nego(rdp, s))
			{
				WLog_ERR(TAG, "%s - rdp_server_accept_nego() fail", rdp_get_state_string(rdp));
			}
			else
			{
				SelectedProtocol = nego_get_selected_protocol(rdp->nego);
				settings->RdstlsSecurity = (SelectedProtocol & PROTOCOL_RDSTLS) ? TRUE : FALSE;
				settings->NlaSecurity = (SelectedProtocol & PROTOCOL_HYBRID) ? TRUE : FALSE;
				settings->TlsSecurity = (SelectedProtocol & PROTOCOL_SSL) ? TRUE : FALSE;
				settings->RdpSecurity = (SelectedProtocol == PROTOCOL_RDP) ? TRUE : FALSE;

				if (SelectedProtocol & PROTOCOL_HYBRID)
				{
					SEC_WINNT_AUTH_IDENTITY_INFO* identity =
					    (SEC_WINNT_AUTH_IDENTITY_INFO*)nego_get_identity(rdp->nego);
					sspi_CopyAuthIdentity(&client->identity, identity);
					IFCALLRET(client->Logon, client->authenticated, client, &client->identity,
					          TRUE);
					nego_free_nla(rdp->nego);
				}
				else
				{
					IFCALLRET(client->Logon, client->authenticated, client, &client->identity,
					          FALSE);
				}
				if (rdp_server_transition_to_state(rdp, CONNECTION_STATE_MCS_CREATE_REQUEST))
					ret = STATE_RUN_SUCCESS;
			}
			break;

		case CONNECTION_STATE_NLA:
			WINPR_ASSERT(FALSE); // TODO
			break;

		case CONNECTION_STATE_MCS_CREATE_REQUEST:
			if (!rdp_server_accept_mcs_connect_initial(rdp, s))
			{
				WLog_ERR(TAG,
				         "%s - "
				         "rdp_server_accept_mcs_connect_initial() fail",
				         rdp_get_state_string(rdp));
			}
			else
				ret = STATE_RUN_SUCCESS;

			break;

		case CONNECTION_STATE_MCS_ERECT_DOMAIN:
			if (!rdp_server_accept_mcs_erect_domain_request(rdp, s))
			{
				WLog_ERR(TAG,
				         "%s - "
				         "rdp_server_accept_mcs_erect_domain_request() fail",
				         rdp_get_state_string(rdp));
			}
			else
				ret = STATE_RUN_SUCCESS;

			break;

		case CONNECTION_STATE_MCS_ATTACH_USER:
			if (!rdp_server_accept_mcs_attach_user_request(rdp, s))
			{
				WLog_ERR(TAG,
				         "%s - "
				         "rdp_server_accept_mcs_attach_user_request() fail",
				         rdp_get_state_string(rdp));
			}
			else
				ret = STATE_RUN_SUCCESS;

			break;

		case CONNECTION_STATE_MCS_CHANNEL_JOIN_REQUEST:
			if (!rdp_server_accept_mcs_channel_join_request(rdp, s))
			{
				WLog_ERR(TAG,
				         "%s - "
				         "rdp_server_accept_mcs_channel_join_request() fail",
				         rdp_get_state_string(rdp));
			}
			else
				ret = STATE_RUN_SUCCESS;
			break;

		case CONNECTION_STATE_RDP_SECURITY_COMMENCEMENT:
			ret = STATE_RUN_SUCCESS;

			if (!rdp_server_establish_keys(rdp, s))
			{
				WLog_ERR(TAG,
				         "%s - "
				         "rdp_server_establish_keys() fail",
				         rdp_get_state_string(rdp));
				ret = STATE_RUN_FAILED;
			}

			if (state_run_success(ret))
			{
				if (!rdp_server_transition_to_state(rdp, CONNECTION_STATE_SECURE_SETTINGS_EXCHANGE))
					ret = STATE_RUN_FAILED;
				else if (Stream_GetRemainingLength(s) > 0)
					ret = STATE_RUN_CONTINUE; /* Rerun function */
			}
			break;

		case CONNECTION_STATE_SECURE_SETTINGS_EXCHANGE:
			if (rdp_recv_client_info(rdp, s))
			{
				if (rdp_server_transition_to_state(
				        rdp, CONNECTION_STATE_CONNECT_TIME_AUTO_DETECT_REQUEST))
					ret = STATE_RUN_CONTINUE;
			}
			break;

		case CONNECTION_STATE_CONNECT_TIME_AUTO_DETECT_REQUEST:
		case CONNECTION_STATE_CONNECT_TIME_AUTO_DETECT_RESPONSE:
			ret = peer_recv_handle_auto_detect(client, s);
			break;

		case CONNECTION_STATE_LICENSING:
			ret = peer_recv_handle_licensing(client, s);
			if (ret == STATE_RUN_CONTINUE)
			{
				if (!rdp_server_transition_to_state(
				        rdp, CONNECTION_STATE_MULTITRANSPORT_BOOTSTRAPPING_REQUEST))
					ret = STATE_RUN_FAILED;
			}
			break;

		case CONNECTION_STATE_MULTITRANSPORT_BOOTSTRAPPING_REQUEST:
			if (settings->SupportMultitransport &&
			    ((settings->MultitransportFlags & INITIATE_REQUEST_PROTOCOL_UDPFECR) != 0))
			{
				/* only UDP reliable for now, nobody does lossy UDP (MS-RDPUDP only) these days */
				ret = multitransport_server_request(rdp->multitransport,
				                                    INITIATE_REQUEST_PROTOCOL_UDPFECR);
				switch (ret)
				{
					case STATE_RUN_SUCCESS:
						rdp_server_transition_to_state(
						    rdp, CONNECTION_STATE_MULTITRANSPORT_BOOTSTRAPPING_RESPONSE);
						break;
					case STATE_RUN_CONTINUE:
						/* mismatch on the supported kind of UDP transports */
						rdp_server_transition_to_state(
						    rdp, CONNECTION_STATE_CAPABILITIES_EXCHANGE_DEMAND_ACTIVE);
						break;
					default:
						break;
				}
			}
			else
			{
				if (rdp_server_transition_to_state(
				        rdp, CONNECTION_STATE_CAPABILITIES_EXCHANGE_DEMAND_ACTIVE))
					ret = STATE_RUN_CONTINUE; /* Rerun, initialize next state */
			}
			break;
		case CONNECTION_STATE_MULTITRANSPORT_BOOTSTRAPPING_RESPONSE:
			ret = peer_recv_pdu(client, s);
			break;

		case CONNECTION_STATE_CAPABILITIES_EXCHANGE_DEMAND_ACTIVE:
			ret = rdp_peer_handle_state_demand_active(client);
			break;

		case CONNECTION_STATE_CAPABILITIES_EXCHANGE_MONITOR_LAYOUT:
			if (freerdp_settings_get_bool(settings, FreeRDP_SupportMonitorLayoutPdu))
			{
				MONITOR_DEF* monitors = NULL;

				IFCALL(client->AdjustMonitorsLayout, client);

				/* client supports the monitorLayout PDU, let's send him the monitors if any */
				ret = STATE_RUN_SUCCESS;
				if (freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount) == 0)
				{
					const UINT32 w = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
					const UINT32 h = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
					const rdpMonitor primary = { .x = 0,
						                         .y = 0,
						                         .width = WINPR_ASSERTING_INT_CAST(int32_t, w),
						                         .height = WINPR_ASSERTING_INT_CAST(int32_t, h),
						                         .is_primary = TRUE,
						                         .orig_screen = 0,
						                         .attributes = { .physicalWidth = w,
						                                         .physicalHeight = h,
						                                         .orientation =
						                                             ORIENTATION_LANDSCAPE,
						                                         .desktopScaleFactor = 100,
						                                         .deviceScaleFactor = 100 } };
					if (!freerdp_settings_set_pointer_array(settings, FreeRDP_MonitorDefArray, 0,
					                                        &primary))
						ret = STATE_RUN_FAILED;
					else if (!freerdp_settings_set_uint32(settings, FreeRDP_MonitorCount, 1))
						ret = STATE_RUN_FAILED;
				}
				if (state_run_failed(ret))
				{
				}
				else if (!display_convert_rdp_monitor_to_monitor_def(
				             settings->MonitorCount, settings->MonitorDefArray, &monitors))
				{
					ret = STATE_RUN_FAILED;
				}
				else if (!freerdp_display_send_monitor_layout(rdp->context, settings->MonitorCount,
				                                              monitors))
				{
					ret = STATE_RUN_FAILED;
				}
				else
					ret = STATE_RUN_SUCCESS;
				free(monitors);

				const size_t len = Stream_GetRemainingLength(s);
				if (!state_run_failed(ret) && (len > 0))
					ret = STATE_RUN_CONTINUE;
			}
			else
			{
				const size_t len = Stream_GetRemainingLength(s);
				if (len > 0)
					ret = STATE_RUN_CONTINUE;
				else
					ret = STATE_RUN_SUCCESS;
			}
			if (state_run_success(ret))
			{
				if (!rdp_server_transition_to_state(
				        rdp, CONNECTION_STATE_CAPABILITIES_EXCHANGE_CONFIRM_ACTIVE))
					ret = STATE_RUN_FAILED;
			}
			break;

		case CONNECTION_STATE_CAPABILITIES_EXCHANGE_CONFIRM_ACTIVE:
			/**
			 * During reactivation sequence the client might sent some input or channel data
			 * before receiving the Deactivate All PDU. We need to process them as usual.
			 */
			ret = peer_recv_pdu(client, s);
			break;

		case CONNECTION_STATE_FINALIZATION_SYNC:
			ret = peer_recv_pdu(client, s);
			if (rdp_finalize_is_flag_set(rdp, FINALIZE_CS_SYNCHRONIZE_PDU))
			{
				if (!rdp_server_transition_to_state(rdp, CONNECTION_STATE_FINALIZATION_COOPERATE))
					ret = STATE_RUN_FAILED;
			}
			else
				ret = peer_unexpected_client_message(rdp, FINALIZE_CS_SYNCHRONIZE_PDU);
			break;
		case CONNECTION_STATE_FINALIZATION_COOPERATE:
			ret = peer_recv_pdu(client, s);
			if (rdp_finalize_is_flag_set(rdp, FINALIZE_CS_CONTROL_COOPERATE_PDU))
			{
				if (!rdp_server_transition_to_state(rdp,
				                                    CONNECTION_STATE_FINALIZATION_REQUEST_CONTROL))
					ret = STATE_RUN_FAILED;
			}
			else
				ret = peer_unexpected_client_message(rdp, FINALIZE_CS_CONTROL_COOPERATE_PDU);
			break;
		case CONNECTION_STATE_FINALIZATION_REQUEST_CONTROL:
			ret = peer_recv_pdu(client, s);
			if (rdp_finalize_is_flag_set(rdp, FINALIZE_CS_CONTROL_REQUEST_PDU))
			{
				if (!rdp_send_server_control_granted_pdu(rdp))
					ret = STATE_RUN_FAILED;
				else if (!rdp_server_transition_to_state(
				             rdp, CONNECTION_STATE_FINALIZATION_PERSISTENT_KEY_LIST))
					ret = STATE_RUN_FAILED;
			}
			else
				ret = peer_unexpected_client_message(rdp, FINALIZE_CS_CONTROL_REQUEST_PDU);
			break;
		case CONNECTION_STATE_FINALIZATION_PERSISTENT_KEY_LIST:
			if (freerdp_settings_get_bool(settings, FreeRDP_BitmapCachePersistEnabled) &&
			    !rdp_finalize_is_flag_set(rdp, FINALIZE_DEACTIVATE_REACTIVATE))
			{
				ret = peer_recv_pdu(client, s);

				if (rdp_finalize_is_flag_set(rdp, FINALIZE_CS_PERSISTENT_KEY_LIST_PDU))
				{
					if (!rdp_server_transition_to_state(rdp,
					                                    CONNECTION_STATE_FINALIZATION_FONT_LIST))
						ret = STATE_RUN_FAILED;
				}
				else
					ret = peer_unexpected_client_message(rdp,
					                                     CONNECTION_STATE_FINALIZATION_FONT_LIST);
			}
			else
			{
				if (!rdp_server_transition_to_state(rdp, CONNECTION_STATE_FINALIZATION_FONT_LIST))
					ret = STATE_RUN_FAILED;
				else
					ret = STATE_RUN_CONTINUE;
			}
			break;
		case CONNECTION_STATE_FINALIZATION_FONT_LIST:
			ret = peer_recv_pdu(client, s);
			if (state_run_success(ret))
			{
				if (rdp_finalize_is_flag_set(rdp, FINALIZE_CS_FONT_LIST_PDU))
				{
					if (!rdp_server_transition_to_state(rdp, CONNECTION_STATE_ACTIVE))
						ret = STATE_RUN_FAILED;
					else
					{
						update_reset_state(rdp->update);
						ret = STATE_RUN_CONTINUE;
					}
				}
				else
					ret = peer_unexpected_client_message(rdp, FINALIZE_CS_FONT_LIST_PDU);
			}
			break;

		case CONNECTION_STATE_ACTIVE:
			ret = rdp_peer_handle_state_active(client);
			if (ret >= STATE_RUN_ACTIVE)
				ret = peer_recv_pdu(client, s);
			break;

			/* States that must not happen in server state machine */
		case CONNECTION_STATE_FINALIZATION_CLIENT_SYNC:
		case CONNECTION_STATE_FINALIZATION_CLIENT_COOPERATE:
		case CONNECTION_STATE_FINALIZATION_CLIENT_GRANTED_CONTROL:
		case CONNECTION_STATE_FINALIZATION_CLIENT_FONT_MAP:
		default:
			WLog_ERR(TAG, "%s state %d", rdp_get_state_string(rdp), rdp_get_state(rdp));
			break;
	}

	return ret;
}

static state_run_t peer_recv_callback(rdpTransport* transport, wStream* s, void* extra)
{
	char buffer[64] = { 0 };
	state_run_t rc = STATE_RUN_FAILED;
	const size_t start = Stream_GetPosition(s);
	const rdpContext* context = transport_get_context(transport);
	DWORD level = WLOG_TRACE;
	static wLog* log = NULL;
	if (!log)
		log = WLog_Get(TAG);

	WINPR_ASSERT(context);
	do
	{
		const rdpRdp* rdp = context->rdp;
		const char* old = rdp_get_state_string(rdp);

		if (rc == STATE_RUN_TRY_AGAIN)
			Stream_SetPosition(s, start);
		rc = peer_recv_callback_internal(transport, s, extra);

		const size_t len = Stream_GetRemainingLength(s);
		if ((len > 0) && !state_run_continue(rc))
			level = WLOG_WARN;
		WLog_Print(log, level,
		           "(server)[%s -> %s] current return %s [%" PRIuz " bytes not processed]", old,
		           rdp_get_state_string(rdp), state_run_result_string(rc, buffer, sizeof(buffer)),
		           len);
	} while (state_run_continue(rc));

	return rc;
}

static BOOL freerdp_peer_close(freerdp_peer* client)
{
	UINT32 SelectedProtocol = 0;
	rdpContext* context = NULL;

	WINPR_ASSERT(client);

	context = client->context;
	WINPR_ASSERT(context);
	WINPR_ASSERT(context->settings);
	WINPR_ASSERT(context->rdp);

	/** if negotiation has failed, we're not MCS connected. So don't
	 * 	send anything else, or some mstsc will consider that as an error
	 */
	SelectedProtocol = nego_get_selected_protocol(context->rdp->nego);

	if (SelectedProtocol & PROTOCOL_FAILED_NEGO)
		return TRUE;

	/**
	 * [MS-RDPBCGR] 1.3.1.4.2 User-Initiated Disconnection Sequence on Server
	 * The server first sends the client a Deactivate All PDU followed by an
	 * optional MCS Disconnect Provider Ultimatum PDU.
	 */
	if (!rdp_send_deactivate_all(context->rdp))
		return FALSE;

	if (freerdp_settings_get_bool(context->settings, FreeRDP_SupportErrorInfoPdu))
	{
		rdp_send_error_info(context->rdp);
	}

	return mcs_send_disconnect_provider_ultimatum(context->rdp->mcs,
	                                              Disconnect_Ultimatum_provider_initiated);
}

static void freerdp_peer_disconnect(freerdp_peer* client)
{
	rdpTransport* transport = NULL;
	WINPR_ASSERT(client);

	transport = freerdp_get_transport(client->context);
	transport_disconnect(transport);
}

static BOOL freerdp_peer_send_channel_data(freerdp_peer* client, UINT16 channelId, const BYTE* data,
                                           size_t size)
{
	WINPR_ASSERT(client);
	WINPR_ASSERT(client->context);
	WINPR_ASSERT(client->context->rdp);
	return rdp_send_channel_data(client->context->rdp, channelId, data, size);
}

static BOOL freerdp_peer_send_server_redirection_pdu(freerdp_peer* peer,
                                                     const rdpRedirection* redirection)
{
	BOOL rc = FALSE;
	WINPR_ASSERT(peer);
	WINPR_ASSERT(peer->context);

	UINT16 sec_flags = 0;
	wStream* s = rdp_send_stream_pdu_init(peer->context->rdp, &sec_flags);
	if (!s)
		return FALSE;
	if (!rdp_write_enhanced_security_redirection_packet(s, redirection))
		goto fail;
	if (!rdp_send_pdu(peer->context->rdp, s, PDU_TYPE_SERVER_REDIRECTION, 0, sec_flags))
		goto fail;
	rc = rdp_reset_runtime_settings(peer->context->rdp);
fail:
	Stream_Release(s);
	return rc;
}

static BOOL freerdp_peer_send_channel_packet(freerdp_peer* client, UINT16 channelId,
                                             size_t totalSize, UINT32 flags, const BYTE* data,
                                             size_t chunkSize)
{
	WINPR_ASSERT(client);
	WINPR_ASSERT(client->context);
	WINPR_ASSERT(client->context->rdp);
	return rdp_channel_send_packet(client->context->rdp, channelId, totalSize, flags, data,
	                               chunkSize);
}

static BOOL freerdp_peer_is_write_blocked(freerdp_peer* peer)
{
	rdpTransport* transport = NULL;
	WINPR_ASSERT(peer);
	WINPR_ASSERT(peer->context);
	WINPR_ASSERT(peer->context->rdp);
	WINPR_ASSERT(peer->context->rdp->transport);
	transport = peer->context->rdp->transport;
	return transport_is_write_blocked(transport);
}

static int freerdp_peer_drain_output_buffer(freerdp_peer* peer)
{
	rdpTransport* transport = NULL;
	WINPR_ASSERT(peer);
	WINPR_ASSERT(peer->context);
	WINPR_ASSERT(peer->context->rdp);
	WINPR_ASSERT(peer->context->rdp->transport);
	transport = peer->context->rdp->transport;
	return transport_drain_output_buffer(transport);
}

static BOOL freerdp_peer_has_more_to_read(freerdp_peer* peer)
{
	WINPR_ASSERT(peer);
	WINPR_ASSERT(peer->context);
	WINPR_ASSERT(peer->context->rdp);
	return transport_have_more_bytes_to_read(peer->context->rdp->transport);
}

static LicenseCallbackResult freerdp_peer_nolicense(freerdp_peer* peer,
                                                    WINPR_ATTR_UNUSED wStream* s)
{
	rdpRdp* rdp = NULL;

	WINPR_ASSERT(peer);
	WINPR_ASSERT(peer->context);

	rdp = peer->context->rdp;

	if (!license_send_valid_client_error_packet(rdp))
	{
		WLog_ERR(TAG, "freerdp_peer_nolicense: license_send_valid_client_error_packet() failed");
		return LICENSE_CB_ABORT;
	}

	return LICENSE_CB_COMPLETED;
}

BOOL freerdp_peer_context_new(freerdp_peer* client)
{
	return freerdp_peer_context_new_ex(client, NULL);
}

void freerdp_peer_context_free(freerdp_peer* client)
{
	if (!client)
		return;

	IFCALL(client->ContextFree, client, client->context);

	if (client->context)
	{
		rdpContext* ctx = client->context;

		(void)CloseHandle(ctx->channelErrorEvent);
		ctx->channelErrorEvent = NULL;
		free(ctx->errorDescription);
		ctx->errorDescription = NULL;
		rdp_free(ctx->rdp);
		ctx->rdp = NULL;
		metrics_free(ctx->metrics);
		ctx->metrics = NULL;
		stream_dump_free(ctx->dump);
		ctx->dump = NULL;
		free(ctx);
	}
	client->context = NULL;
}

static const char* os_major_type_to_string(UINT16 osMajorType)
{
	switch (osMajorType)
	{
		case OSMAJORTYPE_UNSPECIFIED:
			return "Unspecified platform";
		case OSMAJORTYPE_WINDOWS:
			return "Windows platform";
		case OSMAJORTYPE_OS2:
			return "OS/2 platform";
		case OSMAJORTYPE_MACINTOSH:
			return "Macintosh platform";
		case OSMAJORTYPE_UNIX:
			return "UNIX platform";
		case OSMAJORTYPE_IOS:
			return "iOS platform";
		case OSMAJORTYPE_OSX:
			return "OS X platform";
		case OSMAJORTYPE_ANDROID:
			return "Android platform";
		case OSMAJORTYPE_CHROME_OS:
			return "Chrome OS platform";
		default:
			break;
	}

	return "Unknown platform";
}

const char* freerdp_peer_os_major_type_string(freerdp_peer* client)
{
	WINPR_ASSERT(client);

	rdpContext* context = client->context;
	WINPR_ASSERT(context);
	WINPR_ASSERT(context->settings);

	const UINT32 osMajorType = freerdp_settings_get_uint32(context->settings, FreeRDP_OsMajorType);
	WINPR_ASSERT(osMajorType <= UINT16_MAX);
	return os_major_type_to_string((UINT16)osMajorType);
}

static const char* os_minor_type_to_string(UINT16 osMinorType)
{
	switch (osMinorType)
	{
		case OSMINORTYPE_UNSPECIFIED:
			return "Unspecified version";
		case OSMINORTYPE_WINDOWS_31X:
			return "Windows 3.1x";
		case OSMINORTYPE_WINDOWS_95:
			return "Windows 95";
		case OSMINORTYPE_WINDOWS_NT:
			return "Windows NT";
		case OSMINORTYPE_OS2_V21:
			return "OS/2 2.1";
		case OSMINORTYPE_POWER_PC:
			return "PowerPC";
		case OSMINORTYPE_MACINTOSH:
			return "Macintosh";
		case OSMINORTYPE_NATIVE_XSERVER:
			return "Native X Server";
		case OSMINORTYPE_PSEUDO_XSERVER:
			return "Pseudo X Server";
		case OSMINORTYPE_WINDOWS_RT:
			return "Windows RT";
		default:
			break;
	}

	return "Unknown version";
}

const char* freerdp_peer_os_minor_type_string(freerdp_peer* client)
{
	WINPR_ASSERT(client);

	rdpContext* context = client->context;
	WINPR_ASSERT(context);
	WINPR_ASSERT(context->settings);

	const UINT32 osMinorType = freerdp_settings_get_uint32(context->settings, FreeRDP_OsMinorType);
	WINPR_ASSERT(osMinorType <= UINT16_MAX);
	return os_minor_type_to_string((UINT16)osMinorType);
}

freerdp_peer* freerdp_peer_new(int sockfd)
{
	UINT32 option_value = 0;
	socklen_t option_len = 0;
	freerdp_peer* client = (freerdp_peer*)calloc(1, sizeof(freerdp_peer));

	if (!client)
		return NULL;

	option_value = TRUE;
	option_len = sizeof(option_value);

	if (sockfd >= 0)
	{
		if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (void*)&option_value, option_len) < 0)
		{
			/* local unix sockets don't have the TCP_NODELAY implemented, so don't make this
			 * error fatal */
			WLog_DBG(TAG, "can't set TCP_NODELAY, continuing anyway");
		}
	}

	if (client)
	{
		client->sockfd = sockfd;
		client->ContextSize = sizeof(rdpContext);
		client->Initialize = freerdp_peer_initialize;
#if defined(WITH_FREERDP_DEPRECATED)
		client->GetFileDescriptor = freerdp_peer_get_fds;
#endif
		client->GetEventHandle = freerdp_peer_get_event_handle;
		client->GetEventHandles = freerdp_peer_get_event_handles;
		client->CheckFileDescriptor = freerdp_peer_check_fds;
		client->Close = freerdp_peer_close;
		client->Disconnect = freerdp_peer_disconnect;
		client->SendChannelData = freerdp_peer_send_channel_data;
		client->SendChannelPacket = freerdp_peer_send_channel_packet;
		client->SendServerRedirection = freerdp_peer_send_server_redirection_pdu;
		client->IsWriteBlocked = freerdp_peer_is_write_blocked;
		client->DrainOutputBuffer = freerdp_peer_drain_output_buffer;
		client->HasMoreToRead = freerdp_peer_has_more_to_read;
		client->VirtualChannelOpen = freerdp_peer_virtual_channel_open;
		client->VirtualChannelClose = freerdp_peer_virtual_channel_close;
		client->VirtualChannelWrite = freerdp_peer_virtual_channel_write;
		client->VirtualChannelRead = NULL; /* must be defined by server application */
		client->VirtualChannelGetData = freerdp_peer_virtual_channel_get_data;
		client->VirtualChannelSetData = freerdp_peer_virtual_channel_set_data;
		client->SetState = freerdp_peer_set_state;
	}

	return client;
}

void freerdp_peer_free(freerdp_peer* client)
{
	if (!client)
		return;

	sspi_FreeAuthIdentity(&client->identity);
	if (client->sockfd >= 0)
		closesocket((SOCKET)client->sockfd);
	free(client);
}

static BOOL freerdp_peer_transport_setup(freerdp_peer* client)
{
	rdpRdp* rdp = NULL;

	WINPR_ASSERT(client);
	WINPR_ASSERT(client->context);

	rdp = client->context->rdp;
	WINPR_ASSERT(rdp);

	if (!transport_attach(rdp->transport, client->sockfd))
		return FALSE;
	client->sockfd = -1;

	if (!transport_set_recv_callbacks(rdp->transport, peer_recv_callback, client))
		return FALSE;

	if (!transport_set_blocking_mode(rdp->transport, FALSE))
		return FALSE;

	return TRUE;
}

BOOL freerdp_peer_context_new_ex(freerdp_peer* client, const rdpSettings* settings)
{
	rdpRdp* rdp = NULL;
	rdpContext* context = NULL;
	BOOL ret = TRUE;

	if (!client)
		return FALSE;

	WINPR_ASSERT(client->ContextSize >= sizeof(rdpContext));
	if (!(context = (rdpContext*)calloc(1, client->ContextSize)))
		goto fail;

	client->context = context;
	context->peer = client;
	context->ServerMode = TRUE;
	context->log = WLog_Get(TAG);
	if (!context->log)
		goto fail;

	if (settings)
	{
		context->settings = freerdp_settings_clone(settings);
		if (!context->settings)
			goto fail;
	}

	context->dump = stream_dump_new();
	if (!context->dump)
		goto fail;
	if (!(context->metrics = metrics_new(context)))
		goto fail;

	if (!(rdp = rdp_new(context)))
		goto fail;

	rdp_log_build_warnings(rdp);

#if defined(WITH_FREERDP_DEPRECATED)
	client->update = rdp->update;
	client->settings = rdp->settings;
	client->autodetect = rdp->autodetect;
#endif
	context->rdp = rdp;
	context->input = rdp->input;
	context->update = rdp->update;
	context->settings = rdp->settings;
	context->autodetect = rdp->autodetect;
	update_register_server_callbacks(rdp->update);
	autodetect_register_server_callbacks(rdp->autodetect);

	if (!(context->channelErrorEvent = CreateEvent(NULL, TRUE, FALSE, NULL)))
	{
		WLog_ERR(TAG, "CreateEvent failed!");
		goto fail;
	}

	if (!(context->errorDescription = calloc(1, 500)))
	{
		WLog_ERR(TAG, "calloc failed!");
		goto fail;
	}

	if (!freerdp_peer_transport_setup(client))
		goto fail;

	client->IsWriteBlocked = freerdp_peer_is_write_blocked;
	client->DrainOutputBuffer = freerdp_peer_drain_output_buffer;
	client->HasMoreToRead = freerdp_peer_has_more_to_read;
	client->LicenseCallback = freerdp_peer_nolicense;
	IFCALLRET(client->ContextNew, ret, client, client->context);

	if (!ret)
		goto fail;
	return TRUE;

fail:
	WLog_ERR(TAG, "ContextNew callback failed");
	freerdp_peer_context_free(client);
	return FALSE;
}
