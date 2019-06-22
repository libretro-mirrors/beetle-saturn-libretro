#ifndef __INPUT_H__
#define __INPUT_H__

#include "libretro.h"
#include "mednafen/state.h"

// These input routines tell libretro about Saturn peripherals
// and map input from the abstract 'retropad' into Saturn land.

extern void input_init_env( retro_environment_t environ_cb );

extern void input_init();

extern void input_set_geometry( unsigned width, unsigned height );

extern void input_set_env( retro_environment_t environ_cb );

extern void input_set_deadzone_stick( int percent );
extern void input_set_deadzone_trigger( int percent );
extern void input_set_mouse_sensitivity( int percent );

extern void input_update(bool supports_bitmasks,
      retro_input_state_t input_state_cb );

// save state function for input
extern int input_StateAction( StateMem* sm, const unsigned load, const bool data_only );

extern void input_multitap( int port, bool enabled );

#endif
