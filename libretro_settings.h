#ifndef __LIBRETRO_SETTINGS_HDR__
#define __LIBRETRO_SETTINGS_HDR__

enum
{
	SETTING_GUN_CROSSHAIR_OFF,
	SETTING_GUN_CROSSHAIR_CROSS,
	SETTING_GUN_CROSSHAIR_DOT,

	SETTING_GUN_CROSSHAIR_LAST,
};

extern int setting_region;
extern int setting_cart;
extern bool setting_smpc_autortc;
extern int setting_smpc_autortc_lang;
extern int setting_initial_scanline;
extern int setting_initial_scanline_pal;
extern int setting_last_scanline;
extern int setting_last_scanline_pal;
extern int setting_gun_crosshair;
extern bool setting_disc_test;
extern bool setting_multitap_port1;
extern bool setting_multitap_port2;
extern bool setting_midsync;

#endif
