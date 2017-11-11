
#include "libretro.h"
#include "mednafen/mednafen-types.h"
#include "mednafen/ss/ss.h"
#include "mednafen/ss/smpc.h"
#include <math.h>
#include <stdio.h>

//------------------------------------------------------------------------------
// Locals
//------------------------------------------------------------------------------

static retro_environment_t environ_cb; // cached during input_set_env

#define MAX_CONTROLLERS		2

static unsigned players = MAX_CONTROLLERS;

static int astick_deadzone = 0;

typedef union
{
	uint8_t u8[ 32 ];
	uint16_t gun_pos[ 2 ];
	uint16_t buttons;
}
INPUT_DATA;

// Controller state buffer (per player)
static INPUT_DATA input_data[ MAX_CONTROLLERS ] = {0};

// Controller type (per player)
static uint32_t input_type[ MAX_CONTROLLERS ] = {0};


#define INPUT_MODE_3D_PAD_ANALOG		( 1 << 0 ) // Set means analog mode.
#define INPUT_MODE_3D_PAD_PREVIOUS_MASK	( 1 << 1 ) // Edge trigger helper.

#define INPUT_MODE_DEFAULT				0
#define INPUT_MODE_DEFAULT_3D_PAD		INPUT_MODE_3D_PAD_ANALOG

// Mode switch for 3D Control Pad (per player)
static uint32_t input_mode[ MAX_CONTROLLERS ] = {0};



//------------------------------------------------------------------------------
// Supported Devices
//------------------------------------------------------------------------------

#define RETRO_DEVICE_SS_PAD			RETRO_DEVICE_SUBCLASS( RETRO_DEVICE_JOYPAD, 0 )
#define RETRO_DEVICE_SS_3D_PAD		RETRO_DEVICE_SUBCLASS( RETRO_DEVICE_ANALOG, 0 )
#define RETRO_DEVICE_SS_WHEEL		RETRO_DEVICE_SUBCLASS( RETRO_DEVICE_ANALOG, 1 )
#define RETRO_DEVICE_SS_GUN			RETRO_DEVICE_SUBCLASS( RETRO_DEVICE_LIGHTGUN, 0 )

enum { INPUT_DEVICE_TYPES_COUNT = 1 /*none*/ + 4 }; // <-- update me!

static const struct retro_controller_description input_device_types[ INPUT_DEVICE_TYPES_COUNT ] =
{
	{ "Control Pad", RETRO_DEVICE_JOYPAD },
	{ "3D Control Pad", RETRO_DEVICE_SS_3D_PAD },
	{ "Arcade Racer", RETRO_DEVICE_SS_WHEEL },
	{ "Virtua Gun", RETRO_DEVICE_SS_GUN },
	{ NULL, 0 },
};


//------------------------------------------------------------------------------
// Mapping Helpers
//------------------------------------------------------------------------------

/* Control Pad (default) */
enum { INPUT_MAP_PAD_SIZE = 12 };
static const unsigned input_map_pad[ INPUT_MAP_PAD_SIZE ] =
{
	// libretro input				 at position	|| maps to Saturn		on bit
	//-----------------------------------------------------------------------------
	RETRO_DEVICE_ID_JOYPAD_L,		// L1			-> Z					0
	RETRO_DEVICE_ID_JOYPAD_X,		// X(top)		-> Y					1
	RETRO_DEVICE_ID_JOYPAD_Y,		// Y(left)		-> X					2
	RETRO_DEVICE_ID_JOYPAD_R2,		// R2			-> R					3
	RETRO_DEVICE_ID_JOYPAD_UP,		// Pad-Up		-> Pad-Up				4
	RETRO_DEVICE_ID_JOYPAD_DOWN,	// Pad-Down		-> Pad-Down				5
	RETRO_DEVICE_ID_JOYPAD_LEFT,	// Pad-Left		-> Pad-Left				6
	RETRO_DEVICE_ID_JOYPAD_RIGHT,	// Pad-Right	-> Pad-Right			7
	RETRO_DEVICE_ID_JOYPAD_A,		// A(right)		-> B					8
	RETRO_DEVICE_ID_JOYPAD_R,		// R1			-> C					9
	RETRO_DEVICE_ID_JOYPAD_B,		// B(down)		-> A					10
	RETRO_DEVICE_ID_JOYPAD_START,	// Start		-> Start				11
};

static const unsigned input_map_pad_left_shoulder =
	RETRO_DEVICE_ID_JOYPAD_L2;		// L2			-> L					15

/* 3D Control Pad */
enum { INPUT_MAP_3D_PAD_SIZE = 11 };
static const unsigned input_map_3d_pad[ INPUT_MAP_3D_PAD_SIZE ] =
{
	// libretro input				 at position	|| maps to Saturn		on bit
	//-----------------------------------------------------------------------------
	RETRO_DEVICE_ID_JOYPAD_UP,		// Pad-Up		-> Pad-Up				0
	RETRO_DEVICE_ID_JOYPAD_DOWN,	// Pad-Down		-> Pad-Down				1
	RETRO_DEVICE_ID_JOYPAD_LEFT,	// Pad-Left		-> Pad-Left				2
	RETRO_DEVICE_ID_JOYPAD_RIGHT,	// Pad-Right	-> Pad-Right			3
	RETRO_DEVICE_ID_JOYPAD_A,		// A(right)		-> B					4
	RETRO_DEVICE_ID_JOYPAD_R,		// R1			-> C					5
	RETRO_DEVICE_ID_JOYPAD_B,		// B(down)		-> A					6
	RETRO_DEVICE_ID_JOYPAD_START,	// Start		-> Start				7
	RETRO_DEVICE_ID_JOYPAD_L,		// L1			-> Z					8
	RETRO_DEVICE_ID_JOYPAD_X,		// X(top)		-> Y					9
	RETRO_DEVICE_ID_JOYPAD_Y,		// Y(left)		-> X					10
};

static const unsigned input_map_3d_pad_mode_switch =
	RETRO_DEVICE_ID_JOYPAD_SELECT;

/* Arcade Racer (wheel) */
enum { INPUT_MAP_WHEEL_BITSHIFT = 4 };
enum { INPUT_MAP_WHEEL_SIZE = 7 };
static const unsigned input_map_wheel[ INPUT_MAP_WHEEL_SIZE ] =
{
	// libretro input				 at position	|| maps to Saturn		on bit
	//-----------------------------------------------------------------------------
	RETRO_DEVICE_ID_JOYPAD_A,		// A(right)		-> B					4
	RETRO_DEVICE_ID_JOYPAD_R,		// R1			-> C					5
	RETRO_DEVICE_ID_JOYPAD_B,		// B(down)		-> A					6
	RETRO_DEVICE_ID_JOYPAD_START,	// Start		-> Start				7
	RETRO_DEVICE_ID_JOYPAD_L,		// L1			-> Z					8
	RETRO_DEVICE_ID_JOYPAD_X,		// X(top)		-> Y					9
	RETRO_DEVICE_ID_JOYPAD_Y,		// Y(left)		-> X					10
};

static const unsigned input_map_wheel_shift_left =
	RETRO_DEVICE_ID_JOYPAD_L2;
static const unsigned input_map_wheel_shift_right =
	RETRO_DEVICE_ID_JOYPAD_R2;



//------------------------------------------------------------------------------
// Global Functions
//------------------------------------------------------------------------------

void input_init_env( retro_environment_t _environ_cb )
{
	// Cache this
	environ_cb = _environ_cb;

	struct retro_input_descriptor desc[] =
	{
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "A Button" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "B Button" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "C Button" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "X Button" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Y Button" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "Z Button" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L Button" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R Button" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start Button" },
		{ 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Analog X" },
		{ 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "(3D Pad) Mode Switch" },

		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "A Button" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "B Button" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "C Button" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "X Button" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Y Button" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "Z Button" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L Button" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R Button" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
		{ 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Analog X" },
		{ 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "(3D Pad) Mode Switch" },

		{ 0 },
	};

	environ_cb( RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc );
}

void input_set_env( retro_environment_t environ_cb )
{
	static const struct retro_controller_info ports[ MAX_CONTROLLERS + 1 ] =
	{
		{ input_device_types, INPUT_DEVICE_TYPES_COUNT },
		{ input_device_types, INPUT_DEVICE_TYPES_COUNT },
		{ 0 },
	};

	environ_cb( RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports );
}

void input_init()
{
	// Initialise to default and bind input buffers to SMPC emulation.
	for ( unsigned i = 0; i < MAX_CONTROLLERS; ++i )
	{
		input_type[ i ] = RETRO_DEVICE_JOYPAD;
		input_mode[ i ] = INPUT_MODE_DEFAULT;

		SMPC_SetInput( i, "gamepad", (uint8*)&input_data[ i ] );
	}
}

void input_set_deadzone_stick( int percent )
{
	astick_deadzone = (int)( percent * 0.01f * 0x8000);
}

void input_update( retro_input_state_t input_state_cb )
{
	// For each player (logical controller)
	for ( unsigned iplayer = 0; iplayer < players; ++iplayer )
	{
		INPUT_DATA* p_input = &(input_data[ iplayer ]);

		// reset input
		p_input->buttons = 0;

		// What kind of controller is connected?
		switch ( input_type[ iplayer ] )
		{

		case RETRO_DEVICE_JOYPAD:
		case RETRO_DEVICE_SS_PAD:

			{
				//
				// -- standard control pad buttons + d-pad

				// input_map_pad is configured to quickly map libretro buttons to the correct bits for the Saturn.
				for ( int i = 0; i < INPUT_MAP_PAD_SIZE; ++i ) {
					p_input->buttons |= input_state_cb( iplayer, RETRO_DEVICE_JOYPAD, 0, input_map_pad[ i ] ) ? ( 1 << i ) : 0;
				}
				// .. the left trigger on the Saturn is a special case since there's a gap in the bits.
				p_input->buttons |=
					input_state_cb( iplayer, RETRO_DEVICE_JOYPAD, 0, input_map_pad_left_shoulder ) ? ( 1 << 15 ) : 0;
			}
			break;

		case RETRO_DEVICE_SS_3D_PAD:

			{
				//
				// -- 3d control pad buttons

				// input_map_3d_pad is configured to quickly map libretro buttons to the correct bits for the Saturn.
				for ( int i = 0; i < INPUT_MAP_3D_PAD_SIZE; ++i ) {
					p_input->buttons |= input_state_cb( iplayer, RETRO_DEVICE_JOYPAD, 0, input_map_3d_pad[ i ] ) ? ( 1 << i ) : 0;
				}

				//
				// -- analog stick

				int analog_x = input_state_cb( iplayer, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
					RETRO_DEVICE_ID_ANALOG_X );

				int analog_y = input_state_cb( iplayer, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
					RETRO_DEVICE_ID_ANALOG_Y );

				// Analog stick deadzone (borrowed code from parallel-n64 core)
				if ( astick_deadzone > 0 )
				{
					static const int ASTICK_MAX = 0x8000;

					// Convert cartesian coordinate analog stick to polar coordinates
					double radius = sqrt(analog_x * analog_x + analog_y * analog_y);
					double angle = atan2(analog_y, analog_x);

					if (radius > astick_deadzone)
					{
						// Re-scale analog stick range to negate deadzone (makes slow movements possible)
						radius = (radius - astick_deadzone)*((float)ASTICK_MAX/(ASTICK_MAX - astick_deadzone));

						// Convert back to cartesian coordinates
						analog_x = (int)round(radius * cos(angle));
						analog_y = (int)round(radius * sin(angle));

						// Clamp to correct range
						if (analog_x > +32767) analog_x = +32767;
						if (analog_x < -32767) analog_x = -32767;
						if (analog_y > +32767) analog_y = +32767;
						if (analog_y < -32767) analog_y = -32767;
					}
					else
					{
						analog_x = 0;
						analog_y = 0;
					}
				}

				//
				// -- triggers

				// note: LibRetro doesn't support analog triggers, so we must make do with digital inputs for now.
				uint16_t l_trigger = input_state_cb( iplayer, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2) ? 32767 : 0;
				uint16_t r_trigger = input_state_cb( iplayer, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2) ? 32767 : 0;

				//
				// -- mode switch

				{
					// Handle MODE button as a switch
					unsigned prev = ( input_mode[iplayer] & INPUT_MODE_3D_PAD_PREVIOUS_MASK );
					unsigned held = input_state_cb( iplayer, RETRO_DEVICE_JOYPAD, 0, input_map_3d_pad_mode_switch )
						? INPUT_MODE_3D_PAD_PREVIOUS_MASK : 0;

					// Rising edge trigger
					if ( !prev && held )
					{
						// Toggle 'state' bit: analog/digital mode
						input_mode[ iplayer ] ^= INPUT_MODE_3D_PAD_ANALOG;

						// Tell user
						char text[ 256 ];
						if ( input_mode[iplayer] & INPUT_MODE_3D_PAD_ANALOG ) {
							sprintf( text, "Controller %u: Analog Mode", (iplayer+1) );
						} else {
							sprintf( text, "Controller %u: Digital Mode", (iplayer+1) );
						}
						struct retro_message msg = { text, 180 };
						environ_cb( RETRO_ENVIRONMENT_SET_MESSAGE, &msg );
					}

					// Store held state in 'previous' bit.
					input_mode[ iplayer ] = ( input_mode[ iplayer ] & ~INPUT_MODE_3D_PAD_PREVIOUS_MASK ) | held;
				}

				//
				// -- format input data

				// Convert analog values into direction values.
				uint16_t right = analog_x > 0 ?  analog_x : 0;
				uint16_t left  = analog_x < 0 ? -analog_x : 0;
				uint16_t down  = analog_y > 0 ?  analog_y : 0;
				uint16_t up    = analog_y < 0 ? -analog_y : 0;

				// Apply analog/digital mode switch bit.
				if ( input_mode[iplayer] & INPUT_MODE_3D_PAD_ANALOG ) {
					p_input->buttons |= 0x1000; // set bit 12
				}

				p_input->u8[0x2] = ((left  >> 0) & 0xff);
				p_input->u8[0x3] = ((left  >> 8) & 0xff);
				p_input->u8[0x4] = ((right >> 0) & 0xff);
				p_input->u8[0x5] = ((right >> 8) & 0xff);
				p_input->u8[0x6] = ((up    >> 0) & 0xff);
				p_input->u8[0x7] = ((up    >> 8) & 0xff);
				p_input->u8[0x8] = ((down  >> 0) & 0xff);
				p_input->u8[0x9] = ((down  >> 8) & 0xff);
				p_input->u8[0xa] = ((r_trigger >> 0) & 0xff);
				p_input->u8[0xb] = ((r_trigger >> 8) & 0xff);
				p_input->u8[0xc] = ((l_trigger >> 0) & 0xff);
				p_input->u8[0xd] = ((l_trigger >> 8) & 0xff);
			}

			break;

		case RETRO_DEVICE_SS_WHEEL:

			{
				//
				// -- Wheel buttons

				// input_map_wheel is configured to quickly map libretro buttons to the correct bits for the Saturn.
				for ( int i = 0; i < INPUT_MAP_WHEEL_SIZE; ++i ) {
					const uint16_t bit = ( 1 << ( i + INPUT_MAP_WHEEL_BITSHIFT ) );
					p_input->buttons |= input_state_cb( iplayer, RETRO_DEVICE_JOYPAD, 0, input_map_wheel[ i ] ) ? bit : 0;
				}

				// shift-paddles
				p_input->buttons |= input_state_cb( iplayer, RETRO_DEVICE_JOYPAD, 0, input_map_wheel_shift_left ) ? ( 1 << 0 ) : 0;
				p_input->buttons |= input_state_cb( iplayer, RETRO_DEVICE_JOYPAD, 0, input_map_wheel_shift_right ) ? ( 1 << 1 ) : 0;

				//
				// -- analog wheel

				int analog_x = input_state_cb( iplayer, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
					RETRO_DEVICE_ID_ANALOG_X );

				// Analog stick deadzone
				if ( astick_deadzone > 0 )
				{
					static const int ASTICK_MAX = 0x8000;
					const float scale = ((float)ASTICK_MAX/(float)(ASTICK_MAX - astick_deadzone));

					if ( analog_x < -astick_deadzone )
					{
						// Re-scale analog stick range
						float scaled = (-analog_x - astick_deadzone)*scale;

						analog_x = (int)round(-scaled);
						if (analog_x < -32767) {
							analog_x = -32767;
						}
					}
					else if ( analog_x > astick_deadzone )
					{
						// Re-scale analog stick range
						float scaled = (analog_x - astick_deadzone)*scale;

						analog_x = (int)round(scaled);
						if (analog_x > +32767) {
							analog_x = +32767;
						}
					}
					else
					{
						analog_x = 0;
					}
				}

				//
				// -- format input data

				// Convert analog values into direction values.
				uint16_t right = analog_x > 0 ?  analog_x : 0;
				uint16_t left  = analog_x < 0 ? -analog_x : 0;

				p_input->u8[0x2] = ((left  >> 0) & 0xff);
				p_input->u8[0x3] = ((left  >> 8) & 0xff);
				p_input->u8[0x4] = ((right >> 0) & 0xff);
				p_input->u8[0x5] = ((right >> 8) & 0xff);
			}

			break;

		case RETRO_DEVICE_SS_GUN:

			{
				p_input->u8[0x4] = 0;

				// start button
				if ( input_state_cb( iplayer, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_START ) ) {
					p_input->u8[0x4] |= 0x2; // Start
				}

				// trigger
				if ( input_state_cb( iplayer, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER ) ) {
					p_input->u8[0x4] |= 0x1; // Trigger
				}

				// -- Position

				int gun_x_raw, gun_y_raw;
				gun_x_raw = input_state_cb( iplayer, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X ) + 0x8000;
				gun_y_raw = input_state_cb( iplayer, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y ) + 0x8000;

				// .. scale into screen space:
				// NOTE: this is complete hacky guesswork for this first pass, need to re-write.
				int gun_x, gun_y;
				gun_x = ( gun_x_raw * 22000 ) / 65536;
				gun_y = ( gun_y_raw * 240 ) / 65536;

				p_input->gun_pos[ 0 ] = gun_x;
				p_input->gun_pos[ 1 ] = gun_y;

//				log_cb( RETRO_LOG_INFO, "Gun: %d, %d\n", gun_x, gun_y );

				// .. simulated off-screen shot?
				// p_input->u8[0x4] |= 0x4;
			}

		}; // switch ( input_type[ iplayer ] )

	}; // for each player
}


//------------------------------------------------------------------------------
// Libretro Interface
//------------------------------------------------------------------------------

void retro_set_controller_port_device( unsigned in_port, unsigned device )
{
	if ( in_port < MAX_CONTROLLERS )
	{
		// Store input type
		input_type[ in_port ] = device;
		input_mode[ in_port ] = INPUT_MODE_DEFAULT;

		switch ( device )
		{

		case RETRO_DEVICE_NONE:
			log_cb( RETRO_LOG_INFO, "Controller %u: Unplugged\n", (in_port+1) );
			SMPC_SetInput( in_port, "none", (uint8*)&input_data[ in_port ] );
			break;

		case RETRO_DEVICE_JOYPAD:
		case RETRO_DEVICE_SS_PAD:
			log_cb( RETRO_LOG_INFO, "Controller %u: Control Pad\n", (in_port+1) );
			SMPC_SetInput( in_port, "gamepad", (uint8*)&input_data[ in_port ] );
			break;

		case RETRO_DEVICE_SS_3D_PAD:
			log_cb( RETRO_LOG_INFO, "Controller %u: 3D Control Pad\n", (in_port+1) );
			SMPC_SetInput( in_port, "3dpad", (uint8*)&input_data[ in_port ] );
			input_mode[ in_port ] = INPUT_MODE_DEFAULT_3D_PAD;
			break;

		case RETRO_DEVICE_SS_WHEEL:
			log_cb( RETRO_LOG_INFO, "Controller %u: Arcade Racer\n", (in_port+1) );
			SMPC_SetInput( in_port, "wheel", (uint8*)&input_data[ in_port ] );
			break;

		case RETRO_DEVICE_SS_GUN:
			log_cb( RETRO_LOG_INFO, "Controller %u: Virtua Gun\n", (in_port+1) );
			SMPC_SetInput( in_port, "gun", (uint8*)&input_data[ in_port ] );
			break;

		default:
			log_cb( RETRO_LOG_WARN, "Controller %u: Unsupported Device (%u)\n", (in_port+1), device );
			SMPC_SetInput( in_port, "none", (uint8*)&input_data[ in_port ] );
			break;

		}; // switch ( device )

	}; // valid port?
}

//==============================================================================
