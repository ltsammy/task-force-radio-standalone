class RscStructuredText;
class RscPictureKeepAspect;
class RscText;
class RscTitles
{
    class RscTaskForceHint
    {
        idd = 30040;
        onLoad = "uiNamespace setVariable ['TFAR_Hint_Display', _this select 0]";
        onUnload = "uiNamespace setVariable ['TFAR_Hint_Display', displayNull]";
        fadeIn=0.2;
        fadeOut=0.2;
        movingEnable = 0;
        duration = 10e10;
        name = "RscTaskForceHint";
        class controls
        {
            class InformationText: RscStructuredText
            {
                idc = 1100;
                text = "";
                type = 13;
                style = 0;
                x = "0.85 * safezoneW + safezoneX";
                y = "0.9 * safezoneH + safezoneY";
                w = "0.15 * safezoneW";
                h = "0.1 * safezoneH";
                colorText[] = {1,1,1,1};
                colorBackground[] = {0.1,0.1,0.1,0.5};
                sizeEx = 1;
                size = "( ( ( ((safezoneW / safezoneH) min 1.2) / 1.2) / 25) * (0.55 / (getResolution select 5)))";
            };
        };
    };
    class GVAR(HUDVolumeIndicatorRsc) {
        idd = -1;
        movingEnable = 1;
        duration = 9999999;
        fadein = 0;
        fadeout = 0;
        // Mute indicators default hidden -- fnc_updateMuteIndicatorUI.sqf's poll (0.3s, see
        // fnc_ClientInit.sqf) only turns them on when actually muted, but that first poll can't
        // land before this onLoad runs, so set the correct default here rather than flash both
        // on at mission start.
        onLoad = QUOTE(uiNamespace setVariable [ARR_2(QUOTE(QGVAR(HUDVolumeIndicatorRscDisplay)),_this select 0)]; ((_this select 0) displayCtrl 1201) ctrlShow false; ((_this select 0) displayCtrl 1202) ctrlShow false; ((_this select 0) displayCtrl 1203) ctrlShow false;);
        class controls {
            class VolumeIndicator: RscPictureKeepAspect {
                idc= 1112;
                type = 0;
                style = "0x30 + 0x800";
                colorText[] = { 1, 1, 1, 1 };
                colorBackground[]={0, 0, 0, 0};
                font = "PuristaMedium";
                sizeEx = "(((((safezoneW / safezoneH) min 1.2) / 1.2) / 25) * 0.8)";
                text=QPATHTOF(ui\tfar_volume_normal.paa);
                x="(profilenamespace getvariable [""IGUI_grid_TFAR_Volume_X"",	0.85 * safezoneW + safezoneX])";
                y="(profilenamespace getvariable [""IGUI_grid_TFAR_Volume_Y"",	0.9 * safezoneH + safezoneY])";
                w="(profilenamespace getvariable [""IGUI_grid_TFAR_Volume_W"",  2 * (((safezoneW / safezoneH) min 1.2) / 50)])";
                h="(profilenamespace getvariable [""IGUI_grid_TFAR_Volume_W"",  2 * (((safezoneW / safezoneH) min 1.2) / 50)])";
            };
            // Transmitting / mic-mute / speaker-mute status icons, a row immediately left of the
            // volume/range icon. All three default their position OFF the volume icon's own
            // resolved x/y/w (not independent hardcoded coordinates) so they stay correctly
            // grouped with it even if the user has dragged/resized the volume icon via the
            // IGUI_grid_TFAR_Volume_* profileNamespace vars -- each still keeps its own
            // IGUI_grid_TFAR_*_X/Y/W override slot for independent repositioning too.
            // Hidden by default (see onLoad above); fnc_updateMuteIndicatorUI.sqf polls
            // MUTE_STATE's 3 characters (mic muted / speaker muted / actually transmitting --
            // TransmitController's real gate decision, not just "PTT held") to show each one.
            class TransmittingIndicator: RscPictureKeepAspect {
                idc = 1203;
                type = 0;
                style = "0x30 + 0x800";
                colorText[] = { 1, 1, 1, 1 };
                colorBackground[] = {0, 0, 0, 0};
                font = "PuristaMedium";
                sizeEx = "(((((safezoneW / safezoneH) min 1.2) / 1.2) / 25) * 0.8)";
                text = QPATHTOF(ui\tfar_mic_active.paa);
                x="(profilenamespace getvariable [""IGUI_grid_TFAR_Transmitting_X"",	(profilenamespace getvariable [""IGUI_grid_TFAR_Volume_X"",	0.85 * safezoneW + safezoneX]) - 1.3 * (profilenamespace getvariable [""IGUI_grid_TFAR_Volume_W"",  2 * (((safezoneW / safezoneH) min 1.2) / 50)])])";
                y="(profilenamespace getvariable [""IGUI_grid_TFAR_Transmitting_Y"",	(profilenamespace getvariable [""IGUI_grid_TFAR_Volume_Y"",	0.9 * safezoneH + safezoneY])])";
                w="(profilenamespace getvariable [""IGUI_grid_TFAR_Transmitting_W"",  (profilenamespace getvariable [""IGUI_grid_TFAR_Volume_W"",  2 * (((safezoneW / safezoneH) min 1.2) / 50)])])";
                h="(profilenamespace getvariable [""IGUI_grid_TFAR_Transmitting_W"",  (profilenamespace getvariable [""IGUI_grid_TFAR_Volume_W"",  2 * (((safezoneW / safezoneH) min 1.2) / 50)])])";
            };
            class MicMuteIndicator: RscPictureKeepAspect {
                idc = 1201;
                type = 0;
                style = "0x30 + 0x800";
                colorText[] = { 1, 1, 1, 1 };
                colorBackground[] = {0, 0, 0, 0};
                font = "PuristaMedium";
                sizeEx = "(((((safezoneW / safezoneH) min 1.2) / 1.2) / 25) * 0.8)";
                text = QPATHTOF(ui\tfar_mic_muted.paa);
                x="(profilenamespace getvariable [""IGUI_grid_TFAR_MicMute_X"",	(profilenamespace getvariable [""IGUI_grid_TFAR_Volume_X"",	0.85 * safezoneW + safezoneX]) - 2.6 * (profilenamespace getvariable [""IGUI_grid_TFAR_Volume_W"",  2 * (((safezoneW / safezoneH) min 1.2) / 50)])])";
                y="(profilenamespace getvariable [""IGUI_grid_TFAR_MicMute_Y"",	(profilenamespace getvariable [""IGUI_grid_TFAR_Volume_Y"",	0.9 * safezoneH + safezoneY])])";
                w="(profilenamespace getvariable [""IGUI_grid_TFAR_MicMute_W"",  (profilenamespace getvariable [""IGUI_grid_TFAR_Volume_W"",  2 * (((safezoneW / safezoneH) min 1.2) / 50)])])";
                h="(profilenamespace getvariable [""IGUI_grid_TFAR_MicMute_W"",  (profilenamespace getvariable [""IGUI_grid_TFAR_Volume_W"",  2 * (((safezoneW / safezoneH) min 1.2) / 50)])])";
            };
            class SpeakerMuteIndicator: RscPictureKeepAspect {
                idc = 1202;
                type = 0;
                style = "0x30 + 0x800";
                colorText[] = { 1, 1, 1, 1 };
                colorBackground[] = {0, 0, 0, 0};
                font = "PuristaMedium";
                sizeEx = "(((((safezoneW / safezoneH) min 1.2) / 1.2) / 25) * 0.8)";
                text = QPATHTOF(ui\tfar_speaker_muted.paa);
                x="(profilenamespace getvariable [""IGUI_grid_TFAR_SpeakerMute_X"",	(profilenamespace getvariable [""IGUI_grid_TFAR_Volume_X"",	0.85 * safezoneW + safezoneX]) - 3.9 * (profilenamespace getvariable [""IGUI_grid_TFAR_Volume_W"",  2 * (((safezoneW / safezoneH) min 1.2) / 50)])])";
                y="(profilenamespace getvariable [""IGUI_grid_TFAR_SpeakerMute_Y"",	(profilenamespace getvariable [""IGUI_grid_TFAR_Volume_Y"",	0.9 * safezoneH + safezoneY])])";
                w="(profilenamespace getvariable [""IGUI_grid_TFAR_SpeakerMute_W"",  (profilenamespace getvariable [""IGUI_grid_TFAR_Volume_W"",  2 * (((safezoneW / safezoneH) min 1.2) / 50)])])";
                h="(profilenamespace getvariable [""IGUI_grid_TFAR_SpeakerMute_W"",  (profilenamespace getvariable [""IGUI_grid_TFAR_Volume_W"",  2 * (((safezoneW / safezoneH) min 1.2) / 50)])])";
            };
        };
    };
};
