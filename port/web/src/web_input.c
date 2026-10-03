/*
WEB_INPUT.C

Game controllers in the browser. Browsers give gamepads to the page's own
thread only (navigator.getGamepads), not to workers, where the game runs, so
SDL's browser gamepads cannot work here: its callbacks are turned off. The page
copies each controller's state into web_gamepads every animation frame
(port/web/shell/page.js); here each connected one is an SDL virtual gamepad
fed from that state, which the game reads as any other (xinput_sdl.c). The
game's rumble goes back the same way.
*/

#include <stddef.h>
#include <stdint.h>

#include <SDL3/SDL.h>
#include <emscripten/html5.h>

#include "platform.h"

#define WEB_GAMEPAD_COUNT 4
/* the W3C standard gamepad's buttons */
#define WEB_GAMEPAD_BUTTONS 17

struct web_gamepad
{
	/* the page writes these */
	int connected;
	float axes[4];
	float buttons[WEB_GAMEPAD_BUTTONS];
	/* the game writes these: the rumble motors, 0 to 65535 */
	int rumble_low;
	int rumble_high;
};

static struct web_gamepad web_gamepads[WEB_GAMEPAD_COUNT];
static SDL_JoystickID joystick_ids[WEB_GAMEPAD_COUNT];
static SDL_Joystick *joysticks[WEB_GAMEPAD_COUNT];

/* the standard gamepad's buttons as SDL's (the triggers, 6 and 7, are axes) */
static const SDL_GamepadButton standard_buttons[WEB_GAMEPAD_BUTTONS] = {
	SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
	SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
	SDL_GAMEPAD_BUTTON_INVALID, SDL_GAMEPAD_BUTTON_INVALID,
	SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_START,
	SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
	SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
	SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
	SDL_GAMEPAD_BUTTON_GUIDE,
};

/* the page's view of web_gamepads: its address and layout */
EMSCRIPTEN_KEEPALIVE void *web_gamepad_state(int *layout)
{
	layout[0] = (int)sizeof(struct web_gamepad);
	layout[1] = (int)offsetof(struct web_gamepad, connected);
	layout[2] = (int)offsetof(struct web_gamepad, axes);
	layout[3] = (int)offsetof(struct web_gamepad, buttons);
	layout[4] = (int)offsetof(struct web_gamepad, rumble_low);
	layout[5] = (int)offsetof(struct web_gamepad, rumble_high);
	layout[6] = WEB_GAMEPAD_COUNT;
	layout[7] = WEB_GAMEPAD_BUTTONS;
	return web_gamepads;
}

static float clamped(float value, float minimum)
{
	return value < minimum ? minimum : value > 1.0f ? 1.0f : value;
}

static Sint16 stick_value(float value)
{
	value = clamped(value, -1.0f);
	return (Sint16)(value < 0.0f ? value * 32768.0f : value * 32767.0f);
}

/* a trigger's axis spans the joystick's whole range, which SDL's gamepad
mapping turns into 0 (released, -32768) to 32767 */
static Sint16 trigger_value(float value)
{
	return (Sint16)(clamped(value, 0.0f) * 65535.0f - 32768.0f);
}

/* SDL asks for the state as it updates its joysticks (the game's event pump) */
static void SDLCALL gamepad_update(void *userdata)
{
	int index = (int)(intptr_t)userdata;
	const struct web_gamepad *pad = &web_gamepads[index];
	SDL_Joystick *joystick = joysticks[index];
	int axis, button;

	if (!joystick)
		return;
	for (axis = 0; axis < 4; axis++)
		SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX + axis, stick_value(pad->axes[axis]));
	SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, trigger_value(pad->buttons[6]));
	SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, trigger_value(pad->buttons[7]));
	for (button = 0; button < WEB_GAMEPAD_BUTTONS; button++)
	{
		if (standard_buttons[button] != SDL_GAMEPAD_BUTTON_INVALID)
			SDL_SetJoystickVirtualButton(joystick, standard_buttons[button], pad->buttons[button] > 0.5f);
	}
}

static bool SDLCALL gamepad_rumble(void *userdata, Uint16 low_frequency, Uint16 high_frequency)
{
	struct web_gamepad *pad = &web_gamepads[(int)(intptr_t)userdata];

	__atomic_store_n(&pad->rumble_low, (int)low_frequency, __ATOMIC_RELEASE);
	__atomic_store_n(&pad->rumble_high, (int)high_frequency, __ATOMIC_RELEASE);
	return true;
}

/* after SDL starts, on the game's thread */
void web_input_initialize(void)
{
	emscripten_set_gamepadconnected_callback(NULL, 0, NULL);
	emscripten_set_gamepaddisconnected_callback(NULL, 0, NULL);
}

/* each pump of the game's events: a virtual gamepad for each controller the
page has, and none for one it no longer has */
void web_input_update(void)
{
	int index;

	for (index = 0; index < WEB_GAMEPAD_COUNT; index++)
	{
		int connected = __atomic_load_n(&web_gamepads[index].connected, __ATOMIC_ACQUIRE);

		if (connected && !joysticks[index])
		{
			SDL_VirtualJoystickDesc description;
			int button;

			SDL_INIT_INTERFACE(&description);
			description.type = SDL_JOYSTICK_TYPE_GAMEPAD;
			description.naxes = SDL_GAMEPAD_AXIS_COUNT;
			description.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
			for (button = 0; button < WEB_GAMEPAD_BUTTONS; button++)
			{
				if (standard_buttons[button] != SDL_GAMEPAD_BUTTON_INVALID)
					description.button_mask |= 1u << standard_buttons[button];
			}
			description.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
			description.name = "Controller";
			description.userdata = (void *)(intptr_t)index;
			description.Update = gamepad_update;
			description.Rumble = gamepad_rumble;
			joystick_ids[index] = SDL_AttachVirtualJoystick(&description);
			joysticks[index] = joystick_ids[index] ? SDL_OpenJoystick(joystick_ids[index]) : NULL;
			platform_log("controller %d connected", index + 1);
		}
		else if (!connected && joysticks[index])
		{
			SDL_CloseJoystick(joysticks[index]);
			SDL_DetachVirtualJoystick(joystick_ids[index]);
			joysticks[index] = NULL;
			joystick_ids[index] = 0;
			platform_log("controller %d disconnected", index + 1);
		}
	}
}
