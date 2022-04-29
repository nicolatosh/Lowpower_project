#ifndef __etc_H__
#define __etc_H__
/*---------------------------------------------------------------------------*/
#include <stdbool.h>
#include "contiki.h"
#include "net/rime/rime.h"
#include "net/netstack.h"
#include "core/net/linkaddr.h"
/*---------------------------------------------------------------------------*/
#define BEACON_INTERVAL             (CLOCK_SECOND * 30)
#define BEACON_FORWARD_DELAY        (random_rand() % CLOCK_SECOND)
#define EVENT_FORWARD_DELAY         (random_rand() % (CLOCK_SECOND / 10))
#define COLLECT_START_DELAY         (CLOCK_SECOND * 3 + random_rand() % (CLOCK_SECOND))
#define SUPPRESSION_TIMEOUT_NEW     (CLOCK_SECOND * 14)                                 // timeout for new event generation
#define SUPPRESSION_TIMEOUT_PROP    (SUPPRESSION_TIMEOUT_NEW - CLOCK_SECOND / 2)        // timeout for event repropagation
#define SUPPRESSION_TIMEOUT_END     (CLOCK_SECOND / 2)                                  // short timeout after a command to disable suppression
#define MAX_SENSORS                 (10)
#define RSSI_THRESHOLD              (-92)
/*---------------------------------------------------------------------------*/
typedef enum {
  COMMAND_TYPE_NONE,
  COMMAND_TYPE_RESET,
  COMMAND_TYPE_THRESHOLD
} command_type_t;
/*---------------------------------------------------------------------------*/
typedef enum {
  NODE_ROLE_CONTROLLER,
  NODE_ROLE_SENSOR_ACTUATOR,
  NODE_ROLE_FORWARDER
} node_role_t;
/*---------------------------------------------------------------------------*/
/* Callback structure */
struct etc_callbacks {

  /* Controller callbacks */
  void (* recv_cb)(const linkaddr_t *event_source, uint16_t event_seqn, const linkaddr_t *source, uint32_t value, uint32_t threshold);
  void (* ev_cb)(const linkaddr_t *event_source, uint16_t event_seqn);

  /* Sensor/actuator callbacks */
  void (* com_cb)(const linkaddr_t *event_source, uint16_t event_seqn, command_type_t command, uint32_t threshold);
};
/*---------------------------------------------------------------------------*/
/* Data structures for tree management */

/* Tree piece of information at each node */
struct tree_data {
  linkaddr_t parents[2];
  int rssi[2];
  uint16_t metric;
  int beacon_seqn;
  struct ctimer beacon_timer;
};

/* Sensor data payload */
struct sensor_data {
  uint32_t value;
  uint32_t threshold;
  linkaddr_t sensor_addr;
};

/* Connection object */
struct etc_conn {

  /* Connections */
  struct broadcast_conn tree_bc_conn;
  struct tree_data tree_info;
  struct broadcast_conn event_bc_conn;
  struct runicast_conn data_collection_conn;
  struct runicast_conn actuation_conn;
  struct broadcast_conn actuation_opportunistic_conn;
  

  /* Application callbacks */
  const struct etc_callbacks* callbacks;

  /* Timers */
  struct ctimer suppression_timer;      // used to stop the generation of new events
  struct ctimer suppression_prop_timer; // used to temporarily stop the propagation of events from other nodes
  struct ctimer event_prop_timer; // used to send event message after delay
  struct ctimer datacollection_timer; // used to schedule sensor data collection callback
  struct ctimer actuation_timer; // used to schedule the sending of command messages
  struct ctimer actuation_opp_send_timer; // used to start the broadcast forwarding of command messages

  /* Role (controller, forwarder, sensor/actuator) */
  node_role_t node_role;

  /* Current event handled by the node;
   * useful to match logs of the control loop till actuation */
  linkaddr_t event_source;
  uint16_t event_seqn;

  /* Flag conditions */
  bool suppress_internal_event;
  bool suppress_propagation;
  bool suppress_opportunistic; // enable/disable opportunistic fowarding of commands 
};
/*---------------------------------------------------------------------------*/
/* Initialize a ETC connection 
 *  - conn -- a pointer to a ETC connection object 
 *  - channels -- starting channel C (ETC may use multiple channels)
 *  - node_role -- role of the node (controller, forwarders, sensor/actuator)
 *  - callbacks -- a pointer to the callback structure
 *  - sensors -- addresses of sensors
 *  - num_sensors -- how many addresses in sensors */
bool etc_open(
  struct etc_conn* conn,
  uint16_t channels, 
  node_role_t node_role,
  const struct etc_callbacks *callbacks,
  linkaddr_t *sensors,
  uint8_t num_sensors);

/* Sensor functions */
void etc_update(uint32_t value, uint32_t threshold);
int etc_trigger(struct etc_conn *conn, uint32_t value, uint32_t threshold);

/* Controller function */
int etc_command(struct etc_conn *conn, const linkaddr_t *dest, command_type_t command, uint32_t threshold);

/* Close connection */
void etc_close(struct etc_conn *conn);

void data_collection_trigger(void* ptr);

/*---------------------------------------------------------------------------*/
#endif /* __etc_H__ */
