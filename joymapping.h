/*****************************************************************************/
// Handle mapping of various joystick controllers
/*****************************************************************************/

#ifndef JOYMAPPING_H
#define JOYMAPPING_H

#include <inttypes.h>

void map_joystick(uint32_t *map, uint32_t *mmap);
void map_joystick_show(uint32_t *map, uint32_t *mmap, int num);
int map_paddle_btn();

// [MiSTer-DB9 BEGIN] - expose the core's CONF_STR button declarations so
// db9_map.cpp can derive the DB9 factory layout from the core's J1 labels.
// Additive accessors only -- map_joystick()/read_buttons() are untouched.
void        db9_read_default_names();      // refresh from CONF_STR (calls read_buttons)
int         db9_default_name_count();      // joy_count (raw J1 positions, "-" included)
const char *db9_slot_name(int k, int *out_pos); // k-th real button: J1 label + raw pos
// [MiSTer-DB9 END]

#endif // JOYMAPPING_H
