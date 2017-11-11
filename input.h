#ifndef __INPUT_H__
#define __INPUT_H__

// These input routines tell libretro about Saturn peripherals
// and map input from the abstract 'retropad' into Saturn land.

extern void input_init_env( retro_environment_t environ_cb );

extern void input_init();

extern void input_set_env( retro_environment_t environ_cb );

extern void input_set_deadzone_stick( int percent );

extern void input_update( retro_input_state_t input_state_cb );

#endif
