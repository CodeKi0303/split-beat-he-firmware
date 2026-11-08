#ifndef KEYBOARD_HARDWARE_PROFILE_H
#define KEYBOARD_HARDWARE_PROFILE_H
/* Explicit build selection; never probe a different CC controller at runtime. */
#ifndef HARDWARE_REVISION
#define HARDWARE_REVISION 77
#endif
#if HARDWARE_REVISION == 77
#define HARDWARE_NAME "C77"
#define HARDWARE_CC_FUSB303B 0
#elif HARDWARE_REVISION == 85
#define HARDWARE_NAME "C85"
#define HARDWARE_CC_FUSB303B 1
#elif HARDWARE_REVISION == 86
#define HARDWARE_NAME "C86"
#define HARDWARE_CC_FUSB303B 1
#else
#error Unsupported hardware revision
#endif
#endif
