#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * Physical-presence check for the web UI.
 *
 * A browser asks for a challenge: 4 different keys out of 1..9 in random order. While the challenge
 * is pending (max AUTH_CHALLENGE_TTL_S), key presses on the pad are captured by the verifier instead
 * of going to the host. Pressing the 4 keys in order makes the challenge succeed and a session token
 * is issued to the browser that holds the (secret) challenge id. A wrong key fails the challenge;
 * pressing the encoder button cancels it (so nobody on the network can keep the keyboard locked).
 */
#define AUTH_SEQ_LEN        4
#define AUTH_CHALLENGE_TTL_S 60
#define AUTH_ID_LEN         32  /* hex chars */
#define AUTH_TOKEN_LEN      32

typedef enum {
    AUTH_ST_NONE,      /* unknown / superseded challenge */
    AUTH_ST_PENDING,
    AUTH_ST_OK,
    AUTH_ST_FAILED,    /* wrong key */
    AUTH_ST_EXPIRED,
    AUTH_ST_CANCELLED, /* encoder button pressed on the device */
} auth_state_t;

void auth_init(void); /* call once at boot, before the key processor and the web server */

/* New challenge (replaces any pending one). ESP_ERR_INVALID_STATE if a pending challenge is younger than 3 s. */
esp_err_t auth_start(char id[AUTH_ID_LEN + 1], uint8_t keys[AUTH_SEQ_LEN], int *ttl_s);
/* Poll a challenge. When it succeeded, token receives a new session token (only once). */
auth_state_t auth_status(const char *id, int *progress, int *ttl_s, char token[AUTH_TOKEN_LEN + 1]);
void auth_cancel_id(const char *id);

/* Key processor hooks (key index 0..8) */
bool auth_capturing(void);
void auth_key_pressed(int key);
void auth_cancel(void);

/* Sessions */
/* touch: the request counts as user activity and restarts the 10 min idle timer
 * (background polling passes false so an open but unused page still locks). */
bool auth_session_valid(const char *token, bool touch);
void auth_logout(const char *token);
