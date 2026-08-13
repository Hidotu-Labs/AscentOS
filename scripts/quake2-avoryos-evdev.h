/* Direct evdev keyboard fallback for SDL2/KMSDRM on AscentOS. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
typedef struct { uint64_t sec, usec; uint16_t type, code; int32_t value; } ascentos_input_event_t;
static int ascentos_keyboard_fd = -1;
static int AscentOS_TranslateKey(uint16_t c)
{
	static const char letters[59] = {
		[16]='q',[17]='w',[18]='e',[19]='r',[20]='t',[21]='y',[22]='u',[23]='i',[24]='o',[25]='p',
		[30]='a',[31]='s',[32]='d',[33]='f',[34]='g',[35]='h',[36]='j',[37]='k',[38]='l',
		[44]='z',[45]='x',[46]='c',[47]='v',[48]='b',[49]='n',[50]='m'
	};
	if (c < sizeof(letters) && letters[c]) return letters[c];
	if (c >= 2 && c <= 10) return '1' + c - 2;
	switch (c) {
	case 1:return K_ESCAPE; case 11:return '0'; case 12:return '-'; case 13:return '=';
	case 14:return K_BACKSPACE; case 15:return K_TAB; case 26:return '['; case 27:return ']';
	case 28:return K_ENTER; case 29:case 97:return K_CTRL; case 39:return ';'; case 40:return '\'';
	case 41:return K_CONSOLE; case 42:case 54:return K_SHIFT; case 43:return '\\';
	case 51:return ','; case 52:return '.'; case 53:return '/'; case 56:case 100:return K_ALT;
	case 57:return K_SPACE; case 58:return K_CAPSLOCK; case 59:return K_F1; case 60:return K_F2;
	case 61:return K_F3; case 62:return K_F4; case 63:return K_F5; case 64:return K_F6;
	case 65:return K_F7; case 66:return K_F8; case 67:return K_F9; case 68:return K_F10;
	case 87:return K_F11; case 88:return K_F12; case 96:return K_KP_ENTER; case 102:return K_HOME;
	case 103:return K_UPARROW; case 104:return K_PGUP; case 105:return K_LEFTARROW;
	case 106:return K_RIGHTARROW; case 107:return K_END; case 108:return K_DOWNARROW;
	case 109:return K_PGDN; case 110:return K_INS; case 111:return K_DEL; default:return 0;
	}
}
static void AscentOS_KeyboardInit(void)
{
	const char *driver = SDL_GetCurrentVideoDriver();
	if (!driver || SDL_strcasecmp(driver, "KMSDRM") != 0) {
		Com_Printf("AscentOS evdev keyboard: SDL %s backend active\n",
			driver ? driver : "unknown");
		return;
	}
	ascentos_keyboard_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
	if (ascentos_keyboard_fd < 0) Com_Printf("AscentOS evdev keyboard: open failed (%d)\n", errno);
	else Com_Printf("AscentOS evdev keyboard: /dev/input/event0 ready\n");
}
static void AscentOS_KeyboardUpdate(void)
{
	ascentos_input_event_t e; ssize_t n;
	if (ascentos_keyboard_fd < 0) return;
	while ((n = read(ascentos_keyboard_fd, &e, sizeof(e))) == sizeof(e))
		if (e.type == 1) { int key = AscentOS_TranslateKey(e.code); if (key) Key_Event(key, e.value != 0, key >= 128); }
}
static void AscentOS_KeyboardShutdown(void)
{
	if (ascentos_keyboard_fd >= 0) close(ascentos_keyboard_fd);
	ascentos_keyboard_fd = -1;
}
