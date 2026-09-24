#include <string.h>
#include "CyThingEsp32.h"

bool app_command_handle_line(const char *line , int len , int sock)
{
    (void)len;
    (void)sock;

    return false;
}

bool app_command_is_public(const char *line , int len)
{
    (void)line;
    (void)len;

    return false;
}

bool app_mqtt_command_handle(const char *command , int command_length)
{
    return false;
}

void app_main(void)
{
    cything_begin();
}
