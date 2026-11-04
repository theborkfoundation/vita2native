#ifndef CONFIG_H
#define CONFIG_H

#if defined(VITA2NATIVE_LINUX)

#define VITA2NATIVE_ROOT_PATH "vita2native"

#define VITA2HOS_ROOT_PATH        VITA2NATIVE_ROOT_PATH
#define VITA2HOS_EXE_FILE         VITA2HOS_ROOT_PATH "/executable"
#define VITA2HOS_DUMP_PATH        VITA2HOS_ROOT_PATH "/dump"
#define VITA2HOS_DUMP_SHADER_PATH VITA2HOS_DUMP_PATH "/shader"

#else

#define VITA2HOS_ROOT_PATH        "/vita2hos"
#define VITA2HOS_EXE_FILE         VITA2HOS_ROOT_PATH "/executable"
#define VITA2HOS_DUMP_PATH        VITA2HOS_ROOT_PATH "/dump"
#define VITA2HOS_DUMP_SHADER_PATH VITA2HOS_DUMP_PATH "/shader"

#endif

#endif
