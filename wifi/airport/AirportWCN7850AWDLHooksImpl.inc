/* Generated from include/Airport/macOS26/IO80211AWDLProtocol.h: the 63 AWDL hooks. */
static const char *const awdlHookNames[] = {
    "getSSID",
    "getCHANNEL",
    "getBSSID",
    "getSTATE",
    "getAWDL_SYNC_PARAMS",
    "getAWDL_SYNC_ENABLED",
    "getAWDL_EXTENSION_STATE_MACHINE_PARAMETERS",
    "getAWDL_ELECTION_ID",
    "getAWDL_ELECTION_METRIC",
    "getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE",
    "getPEER_CACHE_MAXIMUM_SIZE",
    "getAWDL_MASTER_CHANNEL",
    "getAWDL_SYNC_STATE",
    "getAWDL_ELECTION_RSSI_THRESHOLDS",
    "getAWDL_PRESENCE_MODE",
    "getAWDL_ELECTION_PARAMS",
    "getAWDL_DEVICE_CAPABILITIES",
    "getAWDL_SECONDARY_MASTER_CHANNEL",
    "getAWDL_AF_TX_MODE",
    "getAWDL_MIN_RATE",
    "getAWDL_ENCRYPTION_TYPE",
    "getAWDL_TXCAL_PERIOD",
    "getAWDL_DEV_STATS",
    "getAWDL_DFSP_CONFIG",
    "getAWDL_CCA",
    "getAWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES",
    "getLLW_PARAMS",
    "setSSID",
    "setCHANNEL",
    "setAWDL_SYNC_PARAMS",
    "setAWDL_SYNC_ENABLED",
    "setAWDL_EXTENSION_STATE_MACHINE_PARAMETERS",
    "setAWDL_ELECTION_ID",
    "setAWDL_BSSID",
    "setAWDL_ELECTION_METRIC",
    "setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE",
    "setAWDL_MASTER_CHANNEL",
    "setAWDL_SYNC_STATE",
    "setAWDL_ELECTION_RSSI_THRESHOLDS",
    "setAWDL_PRESENCE_MODE",
    "setPEER_CACHE_CONTROL",
    "setAWDL_OOB_REQUEST",
    "setAWDL_SYNC_FRAME_TEMPLATE",
    "setAWDL_ELECTION_PARAMS",
    "setAWDL_PEER_TRAFFIC_REGISTRATION",
    "setAWDL_SECONDARY_MASTER_CHANNEL",
    "setAWDL_AF_TX_MODE",
    "setAWDL_MIN_RATE",
    "setAWDL_ENCRYPTION_KEYS",
    "setAWDL_ENCRYPTION_TYPE",
    "setAWDL_OOB_AUTO_REQUEST",
    "setAWDL_TXCAL_PERIOD",
    "setAWDL_AWDL_ADVERTISERS",
    "setAWDL_DFSP_CONFIG",
    "setAWDL_DFSP_UCSA_CONFIG",
    "setCOMPANION_SKYWALK_IF_CREATE",
    "setAWDL_OOB_SEC_PAYLOAD_REQUEST",
    "setLLW_PARAMS",
    "setTX_RATE",
    "setP2P_COEX_PROTECTION",
    "setPSF_DWELL",
    "setAWDL_CONFIG",
    "setAWDL_RTG_PEER_STATS_CONFIG",
};
static_assert(sizeof(awdlHookNames) / sizeof(awdlHookNames[0]) == AWDL_HOOK_COUNT, "one name per hook");

IOReturn AirportWCN7850AWDLInterface::getSSID(apple80211_ssid_data *data)
{
    return hook(0, data, false, 16);
}

IOReturn AirportWCN7850AWDLInterface::getCHANNEL(apple80211_channel_data *data)
{
    return hook(1, data, false, 16);
}

IOReturn AirportWCN7850AWDLInterface::getBSSID(apple80211_bssid_data *data)
{
    return hook(2, data, false, 16);
}

IOReturn AirportWCN7850AWDLInterface::getSTATE(apple80211_state_data *data)
{
    return hook(3, data, false, 16);
}

IOReturn AirportWCN7850AWDLInterface::getAWDL_SYNC_PARAMS(apple80211_awdl_sync_params *data)
{
    return hook(4, data, false, 16);
}

IOReturn AirportWCN7850AWDLInterface::getAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled *data)
{
    return hook(5, data, false, 16);
}

IOReturn AirportWCN7850AWDLInterface::getAWDL_EXTENSION_STATE_MACHINE_PARAMETERS(apple80211_awdl_extension_state_machine_parameter *data)
{
    return hook(6, data, false, 16);
}

IOReturn AirportWCN7850AWDLInterface::getAWDL_ELECTION_ID(apple80211_awdl_election_id *data)
{
    return hook(7, data, false, 16);
}

IOReturn AirportWCN7850AWDLInterface::getAWDL_ELECTION_METRIC(apple80211_awdl_election_metric *data)
{
    return hook(8, data, false, 16);
}

IOReturn AirportWCN7850AWDLInterface::getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence *data)
{
    return hook(9, data, false, 16);
}

IOReturn AirportWCN7850AWDLInterface::getPEER_CACHE_MAXIMUM_SIZE(apple80211_peer_cache_maximum_size *data)
{
    return hook(10, data, false, 16);
}

IOReturn AirportWCN7850AWDLInterface::getAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel *data)
{
    return hook(11, data, false, 16);
}

IOReturn AirportWCN7850AWDLInterface::getAWDL_SYNC_STATE(apple80211_awdl_sync_state *data)
{
    return hook(12, data, false, 16);
}

IOReturn AirportWCN7850AWDLInterface::getAWDL_ELECTION_RSSI_THRESHOLDS(apple80211_awdl_election_rssi_thresholds *data)
{
    return hook(13, data, false, 16);
}

IOReturn AirportWCN7850AWDLInterface::getAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode *data)
{
    return hook(14, data, false, 16);
}

IOReturn AirportWCN7850AWDLInterface::getAWDL_ELECTION_PARAMS(apple80211_awdl_election_params *data)
{
    return hook(15, data, false, 16);
}

IOReturn AirportWCN7850AWDLInterface::getAWDL_DEVICE_CAPABILITIES(apple80211_awdl_device_capabilities *data)
{
    return hook(16, data, false, 16);
}

IOReturn AirportWCN7850AWDLInterface::getAWDL_SECONDARY_MASTER_CHANNEL(apple80211_awdl_secondary_master_channel *data)
{
    return hook(17, data, false, 16);
}

IOReturn AirportWCN7850AWDLInterface::getAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode *data)
{
    return hook(18, data, false, 16);
}

IOReturn AirportWCN7850AWDLInterface::getAWDL_MIN_RATE(apple80211_awdl_min_rate *data)
{
    return hook(19, data, false, 16);
}

IOReturn AirportWCN7850AWDLInterface::getAWDL_ENCRYPTION_TYPE(apple80211_encrypt *data)
{
    return hook(20, data, false, 16);
}

IOReturn AirportWCN7850AWDLInterface::getAWDL_TXCAL_PERIOD(apple80211_awdl_txcal_period *data)
{
    return hook(21, data, false, 16);
}

IOReturn AirportWCN7850AWDLInterface::getAWDL_DEV_STATS(apple80211_awdl_dev_stats *data)
{
    return hook(22, data, false, 16);
}

IOReturn AirportWCN7850AWDLInterface::getAWDL_DFSP_CONFIG(apple80211_awdl_dfsp_config *data)
{
    return hook(23, data, false, 16);
}

IOReturn AirportWCN7850AWDLInterface::getAWDL_CCA(apple80211_awdl_cca *data)
{
    return hook(24, data, false, 16);
}

IOReturn AirportWCN7850AWDLInterface::getAWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES(apple80211_awdl_cap *data)
{
    return hook(25, data, false, 16);
}

IOReturn AirportWCN7850AWDLInterface::getLLW_PARAMS(apple80211_llw_params *data)
{
    return hook(26, data, false, 16);
}

IOReturn AirportWCN7850AWDLInterface::setSSID(apple80211_ssid_data *data)
{
    return hook(27, data, true, 16);
}

IOReturn AirportWCN7850AWDLInterface::setCHANNEL(apple80211_channel_data *data)
{
    return hook(28, data, true, 16);
}

IOReturn AirportWCN7850AWDLInterface::setAWDL_SYNC_PARAMS(apple80211_awdl_sync_params *data)
{
    return hook(29, data, true, 64);
}

IOReturn AirportWCN7850AWDLInterface::setAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled *data)
{
    return hook(30, data, true, 16);
}

IOReturn AirportWCN7850AWDLInterface::setAWDL_EXTENSION_STATE_MACHINE_PARAMETERS(apple80211_awdl_extension_state_machine_parameter *data)
{
    return hook(31, data, true, 64);
}

IOReturn AirportWCN7850AWDLInterface::setAWDL_ELECTION_ID(apple80211_awdl_election_id *data)
{
    return hook(32, data, true, 16);
}

IOReturn AirportWCN7850AWDLInterface::setAWDL_BSSID(apple80211_awdl_bssid *data)
{
    return hook(33, data, true, 16);
}

IOReturn AirportWCN7850AWDLInterface::setAWDL_ELECTION_METRIC(apple80211_awdl_election_metric *data)
{
    return hook(34, data, true, 16);
}

IOReturn AirportWCN7850AWDLInterface::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence *data)
{
    return hook(35, data, true, 64);
}

IOReturn AirportWCN7850AWDLInterface::setAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel *data)
{
    return hook(36, data, true, 16);
}

IOReturn AirportWCN7850AWDLInterface::setAWDL_SYNC_STATE(apple80211_awdl_sync_state *data)
{
    return hook(37, data, true, 16);
}

IOReturn AirportWCN7850AWDLInterface::setAWDL_ELECTION_RSSI_THRESHOLDS(apple80211_awdl_election_rssi_thresholds *data)
{
    return hook(38, data, true, 16);
}

IOReturn AirportWCN7850AWDLInterface::setAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode *data)
{
    return hook(39, data, true, 16);
}

IOReturn AirportWCN7850AWDLInterface::setPEER_CACHE_CONTROL(apple80211_peer_cache_control *data)
{
    return hook(40, data, true, 16);
}

IOReturn AirportWCN7850AWDLInterface::setAWDL_OOB_REQUEST(apple80211_awdl_oob_request *data)
{
    return hook(41, data, true, 64);
}

IOReturn AirportWCN7850AWDLInterface::setAWDL_SYNC_FRAME_TEMPLATE(apple80211_awdl_sync_frame_template *data)
{
    return hook(42, data, true, 64);
}

IOReturn AirportWCN7850AWDLInterface::setAWDL_ELECTION_PARAMS(apple80211_awdl_election_params *data)
{
    return hook(43, data, true, 64);
}

IOReturn AirportWCN7850AWDLInterface::setAWDL_PEER_TRAFFIC_REGISTRATION(apple80211_awdl_peer_traffic_registration *data)
{
    return hook(44, data, true, 16);
}

IOReturn AirportWCN7850AWDLInterface::setAWDL_SECONDARY_MASTER_CHANNEL(apple80211_awdl_secondary_master_channel *data)
{
    return hook(45, data, true, 16);
}

IOReturn AirportWCN7850AWDLInterface::setAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode *data)
{
    return hook(46, data, true, 16);
}

IOReturn AirportWCN7850AWDLInterface::setAWDL_MIN_RATE(apple80211_awdl_min_rate *data)
{
    return hook(47, data, true, 16);
}

IOReturn AirportWCN7850AWDLInterface::setAWDL_ENCRYPTION_KEYS(apple80211_key *data)
{
    return hook(48, data, true, 16);
}

IOReturn AirportWCN7850AWDLInterface::setAWDL_ENCRYPTION_TYPE(apple80211_encrypt *data)
{
    return hook(49, data, true, 16);
}

IOReturn AirportWCN7850AWDLInterface::setAWDL_OOB_AUTO_REQUEST(apple80211_awdl_oob_request *data)
{
    return hook(50, data, true, 64);
}

IOReturn AirportWCN7850AWDLInterface::setAWDL_TXCAL_PERIOD(apple80211_awdl_txcal_period *data)
{
    return hook(51, data, true, 16);
}

IOReturn AirportWCN7850AWDLInterface::setAWDL_AWDL_ADVERTISERS(awdl_peer_advert_add *data)
{
    return hook(52, data, true, 16);
}

IOReturn AirportWCN7850AWDLInterface::setAWDL_DFSP_CONFIG(apple80211_awdl_dfsp_config *data)
{
    return hook(53, data, true, 16);
}

IOReturn AirportWCN7850AWDLInterface::setAWDL_DFSP_UCSA_CONFIG(apple80211_awdl_dfsp_ucsa_config *data)
{
    return hook(54, data, true, 16);
}

IOReturn AirportWCN7850AWDLInterface::setCOMPANION_SKYWALK_IF_CREATE(apple80211_companion_skywalk_if_create *data)
{
    return hook(55, data, true, 16);
}

IOReturn AirportWCN7850AWDLInterface::setAWDL_OOB_SEC_PAYLOAD_REQUEST(apple80211_awdl_af_sec_payload_request *data)
{
    return hook(56, data, true, 16);
}

IOReturn AirportWCN7850AWDLInterface::setLLW_PARAMS(apple80211_llw_params *data)
{
    return hook(57, data, true, 16);
}

IOReturn AirportWCN7850AWDLInterface::setTX_RATE(apple80211_tx_rate *data)
{
    return hook(58, data, true, 16);
}

IOReturn AirportWCN7850AWDLInterface::setP2P_COEX_PROTECTION(apple80211_p2p_coex_protection *data)
{
    return hook(59, data, true, 16);
}

IOReturn AirportWCN7850AWDLInterface::setPSF_DWELL(apple80211_awdl_psf_dwell_params *data)
{
    return hook(60, data, true, 16);
}

IOReturn AirportWCN7850AWDLInterface::setAWDL_CONFIG(apple80211_awdl_config_request *data)
{
    return hook(61, data, true, 64);
}

IOReturn AirportWCN7850AWDLInterface::setAWDL_RTG_PEER_STATS_CONFIG(apple80211_rtg_peer_stats_cfg *data)
{
    return hook(62, data, true, 16);
}
