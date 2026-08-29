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
        onLoad = QUOTE(uiNamespace setVariable [ARR_2(QUOTE(QGVAR(HUDVolumeIndicatorRscDisplay)),_this select 0)]; ((_this select 0) displayCtrl 1201) ctrlShow false; ((_this select 0) displayCtrl 1202) ctrlShow false;);
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
            // Mic-mute / speaker-mute hint, stacked above the volume icon. Hidden by default
            // (see onLoad above); fnc_updateMuteIndicatorUI.sqf shows each one only while that
            // specific mute is actually active.
            class MicMuteIndicator: RscText {
                idc = 1201;
                style = "0x02 + 0x800";
                colorText[] = {1, 1, 1, 1};
                colorBackground[] = {0.6, 0.1, 0.1, 0.7};
                font = "PuristaBold";
                text = "$STR_TFAR_HUD_MicMuted";
                sizeEx = "(((((safezoneW / safezoneH) min 1.2) / 1.2) / 32) * 0.8)";
                x="(profilenamespace getvariable [""IGUI_grid_TFAR_MicMute_X"",	0.70 * safezoneW + safezoneX])";
                y="(profilenamespace getvariable [""IGUI_grid_TFAR_MicMute_Y"",	0.84 * safezoneH + safezoneY])";
                w="(profilenamespace getvariable [""IGUI_grid_TFAR_MicMute_W"",  0.14 * safezoneW])";
                h="(profilenamespace getvariable [""IGUI_grid_TFAR_MicMute_H"",  2.4 * (((safezoneW / safezoneH) min 1.2) / 50)])";
            };
            class SpeakerMuteIndicator: RscText {
                idc = 1202;
                style = "0x02 + 0x800";
                colorText[] = {1, 1, 1, 1};
                colorBackground[] = {0.6, 0.1, 0.1, 0.7};
                font = "PuristaBold";
                text = "$STR_TFAR_HUD_SpeakerMuted";
                sizeEx = "(((((safezoneW / safezoneH) min 1.2) / 1.2) / 32) * 0.8)";
                x="(profilenamespace getvariable [""IGUI_grid_TFAR_SpeakerMute_X"",	0.85 * safezoneW + safezoneX])";
                y="(profilenamespace getvariable [""IGUI_grid_TFAR_SpeakerMute_Y"",	0.84 * safezoneH + safezoneY])";
                w="(profilenamespace getvariable [""IGUI_grid_TFAR_SpeakerMute_W"",  0.14 * safezoneW])";
                h="(profilenamespace getvariable [""IGUI_grid_TFAR_SpeakerMute_H"",  2.4 * (((safezoneW / safezoneH) min 1.2) / 50)])";
            };
        };
    };
};
