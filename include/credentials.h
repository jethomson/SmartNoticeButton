/* This header file outlines the credential variables required without containing actual credentials.
 * You may fill in your credentials here or put them in credentials_private.h using this file as a reference.
 */

#if __has_include("credentials_private.h")
#include "credentials_private.h"
#else
const char* wifi_ssid = "YOUR_WIFI_SSID";
const char* wifi_password = "YOUR_WIFI_PASSWORD";
#endif

