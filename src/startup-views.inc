/* Fixed baseline metadata; independent public zero defaults. */
static const struct zte_db_domain startup_P1905ComCfg[] = {
    { .name = "Enable", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "EnableIsSync", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "Policy", .type = 13, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "DropNum", .type = 13, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "MarketConfEn", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "MarketSign", .type = 3, .primary_key = 0, .save = 1, .default_value = "" },
    { .name = "DropRoam", .type = 13, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "A1Enable", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "", .default_value = "0" }
};
static const struct zte_db_domain startup_IPInterface[] = {
    { .name = "ViewName", .type = 2, .primary_key = 1, .save = 1, .default_value = "" },
    { .name = "Enable", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "IPv4Enable", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "IPv6Enable", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "ULAEnable", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "Status", .type = 14, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "Alias", .type = 3, .primary_key = 0, .save = 1, .default_value = "" },
    { .name = "Name", .type = 3, .primary_key = 0, .save = 1, .default_value = "" },
    { .name = "LastChange", .type = 13, .primary_key = 0, .save = 0, .default_value = "0" },
    { .name = "LowerLayers", .type = 3, .primary_key = 0, .save = 1, .default_value = "" },
    { .name = "Router", .type = 5, .primary_key = 0, .save = 1, .default_value = "" },
    { .name = "MaxMTUSize", .type = 13, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "Type", .type = 14, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "Loopback", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "AutoIPEnable", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "IsForwardv4", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "IsForwardv6", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "IsDefGWv4", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "IsDefGWv6", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "IsNat", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "IsBridge", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "tServList", .type = 13, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "WanLanPrority", .type = 13, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "V4Status", .type = 14, .primary_key = 0, .save = 0, .default_value = "0" },
    { .name = "V4RouteStatus", .type = 14, .primary_key = 0, .save = 0, .default_value = "0" },
    { .name = "V6Status", .type = 14, .primary_key = 0, .save = 0, .default_value = "0" },
    { .name = "IPv6AcquireMode", .type = 14, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "SlaacSwitcher", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "Unnumbered", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "IsGUAFromPD", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "PDConfigGUAExpried", .type = 12, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "GataWay6", .type = 17, .primary_key = 0, .save = 1, .default_value = "::" },
    { .name = "WanName", .type = 5, .primary_key = 0, .save = 1, .default_value = "" },
    { .name = "DnsEnabled", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "DnsOverrideAllowed", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "OverridedDNS1", .type = 16, .primary_key = 0, .save = 1, .default_value = "0.0.0.0" },
    { .name = "OverridedDNS2", .type = 16, .primary_key = 0, .save = 1, .default_value = "0.0.0.0" },
    { .name = "OverridedDNS3", .type = 16, .primary_key = 0, .save = 1, .default_value = "0.0.0.0" },
    { .name = "DnsOverrideAllowed6", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "Overridedv6DNS1", .type = 17, .primary_key = 0, .save = 1, .default_value = "::" },
    { .name = "Overridedv6DNS2", .type = 17, .primary_key = 0, .save = 1, .default_value = "::" },
    { .name = "Overridedv6DNS3", .type = 17, .primary_key = 0, .save = 1, .default_value = "::" },
    { .name = "TunnelType", .type = 14, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "GataWay6Status", .type = 17, .primary_key = 0, .save = 0, .default_value = "::" },
    { .name = "V6UpTime", .type = 12, .primary_key = 0, .save = 0, .default_value = "0" },
    { .name = "LLAIFID", .type = 2, .primary_key = 0, .save = 1, .default_value = "" },
    { .name = "GUAIFID", .type = 2, .primary_key = 0, .save = 1, .default_value = "" },
    { .name = "Origin", .type = 12, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "", .default_value = "0" }
};
static const struct zte_db_domain startup_P1905_ACD[] = {
    { .name = "ACDRole", .type = 13, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "ACDVersion", .type = 13, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "ACDSyncEnable", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "SyncCurrentCardParm", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "lastSyncTime", .type = 3, .primary_key = 0, .save = 1, .default_value = "" },
    { .name = "ElectStartTime", .type = 3, .primary_key = 0, .save = 1, .default_value = "" },
    { .name = "ElectReason", .type = 13, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "AcdMsgMaxTmOutTimes", .type = 13, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "SoftVerHeader", .type = 2, .primary_key = 0, .save = 1, .default_value = "" },
    { .name = "", .default_value = "0" }
};
static const struct zte_db_domain startup_P1905AcdBssCfg[] = {
    { .name = "ViewName", .type = 2, .primary_key = 1, .save = 1, .default_value = "" },
    { .name = "WLANCfgInst", .type = 2, .primary_key = 0, .save = 1, .default_value = "" },
    { .name = "WLANBaseName", .type = 2, .primary_key = 0, .save = 1, .default_value = "" },
    { .name = "CustomTag", .type = 13, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "Enable", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "", .default_value = "0" }
};
static const struct zte_db_domain startup_AclCfg[] = {
    { .name = "ViewName", .type = 2, .primary_key = 1, .save = 1, .default_value = "" },
    { .name = "Name", .type = 2, .primary_key = 0, .save = 1, .default_value = "" },
    { .name = "Interface", .type = 2, .primary_key = 0, .save = 1, .default_value = "" },
    { .name = "MACAddress", .type = 18, .primary_key = 0, .save = 1, .default_value = "00:00:00:00:00:00" },
    { .name = "ACLCfgPolicy", .type = 14, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "", .default_value = "0" }
};
static const struct zte_db_domain startup_WLANWEP[] = {
    { .name = "ViewName", .type = 2, .primary_key = 1, .save = 1, .default_value = "" },
    { .name = "WLANViewName", .type = 2, .primary_key = 0, .save = 1, .default_value = "" },
    { .name = "Key", .type = 3, .primary_key = 0, .save = 1, .default_value = "" },
    { .name = "", .default_value = "0" }
};
static const struct zte_db_domain startup_P1905DeviceInfo[] = {
    { .name = "ViewName", .type = 2, .primary_key = 1, .save = 1, .default_value = "" },
    { .name = "MacAddr", .type = 18, .primary_key = 1, .save = 1, .default_value = "00:00:00:00:00:00" },
    { .name = "ManufacturerModel", .type = 3, .primary_key = 0, .save = 1, .default_value = "" },
    { .name = "ModelName", .type = 3, .primary_key = 0, .save = 1, .default_value = "" },
    { .name = "ProductClass", .type = 3, .primary_key = 0, .save = 0, .default_value = "" },
    { .name = "SerialNumber", .type = 3, .primary_key = 0, .save = 0, .default_value = "" },
    { .name = "SoftwareVer", .type = 3, .primary_key = 0, .save = 0, .default_value = "" },
    { .name = "RegistrarFreqBand", .type = 0, .primary_key = 0, .save = 0, .default_value = "" },
    { .name = "ZTEDevFlag", .type = 15, .primary_key = 0, .save = 0, .default_value = "0" },
    { .name = "Active", .type = 15, .primary_key = 0, .save = 0, .default_value = "0" },
    { .name = "Friendlyname", .type = 3, .primary_key = 0, .save = 1, .default_value = "" },
    { .name = "", .default_value = "0" }
};
static const struct zte_db_domain startup_MultiAPRoamCatonCfg[] = {
    { .name = "OfflineRetrieveEn", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "TxDataAfterBaEn", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "TxBaDelayMs", .type = 13, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "ZeroDropCfgEn", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "BtmCacheEn", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "TxConDelayMs", .type = 13, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "TxDropCnt", .type = 13, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "", .default_value = "0" }
};
static const struct zte_db_domain startup_WLANSetting[] = {
    { .name = "ViewName", .type = 2, .primary_key = 1, .save = 1, .default_value = "" },
    { .name = "DualBandSync", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "SSIDSync", .type = 13, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "AirMonitorDuration", .type = 13, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "SpecialMode", .type = 13, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "ESSIDSuffix", .type = 2, .primary_key = 0, .save = 1, .default_value = "" },
    { .name = "ACLIsFrom1905Slave", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "MLOEnable", .type = 15, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "MLOSSID", .type = 13, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "TimerStatus", .type = 13, .primary_key = 0, .save = 0, .default_value = "0" },
    { .name = "DfsReentryEnable", .type = 13, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "MinScanInterval", .type = 13, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "BootScanDuration", .type = 13, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "ScanInterval", .type = 13, .primary_key = 0, .save = 1, .default_value = "0" },
    { .name = "", .default_value = "0" }
};
static const struct zte_db_domain startup_MultiAPCacheMac[] = {
    { .name = "ViewName", .type = 2, .primary_key = 1, .save = 1, .default_value = "" },
    { .name = "MacAddr", .type = 18, .primary_key = 1, .save = 1, .default_value = "00:00:00:00:00:00" },
    { .name = "", .default_value = "0" }
};
