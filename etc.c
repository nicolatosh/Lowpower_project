#include <stdbool.h>
#include "contiki.h"
#include "lib/random.h"
#include "net/rime/rime.h"
#include "leds.h"
#include "net/netstack.h"
#include <stdio.h>
#include "core/net/linkaddr.h"
#include "etc.h"
/*---------------------------------------------------------------------------*/
/* A simple debug system to enable/disable some printfs */
#define DEBUG 0
#if DEBUG
#define PRINTF(...) printf(__VA_ARGS__)
#else
#define PRINTF(...)
#endif
#define SENSORS 5
#define MAX_DATA_RETRANSMISSIONS 4
#define MAX_COMMAND_RETRANSMISSIONS 4
#define EXTRA_DATA_RETRANSMISSIONS 2
#define EXTRA_COMMAND_RETRANSMISSIONS 2
#define DATA_RETRANSMIT_OFFSET (random_rand() % 310)
#define DATA_TRANSMIT_OFFSET (CLOCK_SECOND / 2) 
#define DATA_SENSOR_OFFSET (random_rand() % (CLOCK_SECOND))
#define ACTUATION_RETRANSMIT_OFFSET (random_rand() % 420)
#define ACTUATION_MULTIPLE_SEND_OFFSET (random_rand() % 200)
#define ACTUATION_OPPORTUNISTIC_OFFSET (4 * CLOCK_SECOND) 
#define COLLECT_TRIGGER (random_rand() % (CLOCK_SECOND) + (CLOCK_SECOND/2))
/*---------------------------------------------------------------------------*/
/* Topology information (parents, metrics...) */

void send_beacon(struct etc_conn* conn); 
void send_event(void* ptr);
struct sensor_data sensor_event_data = {.value = 0, .threshold = 0};

/*---------------------------------------------------------------------------*/
/* Forwarders (routes to sensors/actuators) */

linkaddr_t *downward_table_sources;
linkaddr_t downward_table_destinations[SENSORS];

/*---------------------------------------------------------------------------*/
/* Data structures */

/* Structure for TREE BEACONS */
struct beacon_msg {
  uint16_t seqn;
  uint16_t metric;
} __attribute__((packed));

/* Structure for EVENT packets */
struct event_msg_t {
  linkaddr_t event_source;
  uint16_t event_seqn;
};

/* COMMAND message struct */
struct command_msg_t {
  linkaddr_t event_source;
  linkaddr_t dest;
  uint16_t event_seqn;
  command_type_t command;
  uint32_t treshold;
  uint16_t retransmitted;
};

/* Structure for COLLECT packets */
struct collect_msg_t {
  linkaddr_t event_source;
  uint16_t event_seqn;
  struct sensor_data s_data;
  uint16_t retransmitted;
};


struct collect_msg_t last_collect; // last collect message managed by the node
struct command_msg_t last_actuation_command; // last command managed by the node
struct command_msg_t opportunistic_actuation_command; // the command to sent using opp. connection
struct command_msg_t last_opportunistic_actuation_command; // last opp. command received

/*---------------------------------------------------------------------------*/
/* Queues data structures */
struct command_msg_list_t {
  struct command_msg_list_t* next;
  struct command_msg_t actuation_command;
};

struct collect_msg_list_t {
  struct collect_msg_list_t* next;
  struct collect_msg_t collect_message;
};

/* Buffer of COMMAND messages, used by controller only */
LIST(commands_to_send);
MEMB(actuation_mem, struct command_msg_list_t, SENSORS);

/* Buffer per node of COLLECT messages to be delivered */
LIST(collect_to_send);
MEMB(collect_mem, struct collect_msg_list_t, SENSORS);


/*---------------------------------------------------------------------------*/
/* Declarations for the callbacks of dedicated connection objects */

/* Tree */
void tree_bc_recv(struct broadcast_conn *conn, const linkaddr_t *sender);
void tree_beacon_timer_cb(void* ptr);

/* Events */
void event_bc_recv(struct broadcast_conn *conn, const linkaddr_t *sender);
void event_bc_sent(struct broadcast_conn *ptr, int status, int num_tx);

/* Opportunistic commands */
void actuation_opp_bc_recv(struct broadcast_conn *conn, const linkaddr_t *sender);

/* Data Collection */
void data_collection_uc_recv(struct runicast_conn *c, const linkaddr_t *from, uint8_t seqno);
static void sent_data_rc(struct runicast_conn *c, const linkaddr_t *to, uint8_t retransmissions);
static void sent_data_timedout(struct runicast_conn *c, const linkaddr_t *to, uint8_t retransmissions);

/* Commands/actuation */
void actuation_command_uc_recv(struct runicast_conn *c, const linkaddr_t *from, uint8_t seqno);
static void sent_actuation_rc(struct runicast_conn *c, const linkaddr_t *to, uint8_t retransmissions);
static void sent_actuation_timedout(struct runicast_conn *c, const linkaddr_t *to, uint8_t retransmissions);

/*---------------------------------------------------------------------------*/
/* Additional timers */

struct ctimer data_transmission_timer; // schedules the actual send of Collect by sensors
struct ctimer data_retransmission_timer;
struct ctimer actuation_multiple_send_timer; // schedules the send of Command messages at a node
struct ctimer actuation_retransmission_timer;
struct ctimer actuation_opp_suppress_timer;

/* Timers callbacks */
void opportunistic_timer_cb(void *ptr);
void send_actuation_command(void *ptr);
void send_actuation_opportunistic(void *ptr);
void send_event_delayed(void* ptr);
void node_internal_event_cb(void* ptr);
void node_propagation_event_cb(void* ptr);
void data_collection_send(void* ptr);
static void data_collection_resend(void *ptr);
static void actuation_resend(void *ptr);


/*---------------------------------------------------------------------------*/
/* Utils functions */

int find_sensor_index(const linkaddr_t *source);
bool check_collect_message_queue(struct collect_msg_t *message);
bool check_command_message_queue(struct command_msg_t *message);

/*---------------------------------------------------------------------------*/
/* Rime Callback structures */

/* 5 connections:
 *
 * 1. tree 
 * 2. events
 * 3. data collection
 * 4. commands
 * 5. commands in broadcast (opportunistic)
 */

static const struct broadcast_callbacks tree_bc_callback = {
  .recv = tree_bc_recv,
  .sent = NULL
};

static struct broadcast_callbacks event_bc_callback = {
  .recv = event_bc_recv,
  .sent = event_bc_sent
};

static const struct broadcast_callbacks actuation_opportunistic_bc_callback = {
  .recv = actuation_opp_bc_recv,
  .sent = NULL
};

static const struct runicast_callbacks data_collection_callback = {
  .recv = data_collection_uc_recv,
  .sent = sent_data_rc,
  .timedout = sent_data_timedout
};

static const struct runicast_callbacks actuation_callback = {
  .recv = actuation_command_uc_recv,
  .sent = sent_actuation_rc,
  .timedout = sent_actuation_timedout
};
/*---------------------------------------------------------------------------*/
/*                           Application Interface                           */
/*---------------------------------------------------------------------------*/
/* Create connection(s) and start the protocol */
bool
etc_open(struct etc_conn* conn, uint16_t channels, 
         node_role_t node_role, const struct etc_callbacks *callbacks,
         linkaddr_t *sensors, uint8_t num_sensors)
{
  /* Initialize the connector structure */
  conn->node_role = node_role;
  conn->callbacks = callbacks;
  conn->event_seqn = 0;

  /* Initialize the tree data */
  linkaddr_copy(&conn->event_source, &linkaddr_null);
  conn->tree_info.beacon_seqn = 0;
  conn->tree_info.metric = UINT16_MAX;
  conn->tree_info.rssi[0] = -125;
  conn->tree_info.rssi[1] = -125;
  linkaddr_copy(&conn->tree_info.parents[0], &linkaddr_null);
  linkaddr_copy(&conn->tree_info.parents[1], &linkaddr_null);
  conn->suppress_internal_event = false;
  conn->suppress_propagation = false;

  /* Open the underlying Rime primitives */
  broadcast_open(&conn->tree_bc_conn, channels, &tree_bc_callback);
  broadcast_open(&conn->event_bc_conn, channels + 1, &event_bc_callback);
  broadcast_open(&conn->actuation_opportunistic_conn, channels + 2, &actuation_opportunistic_bc_callback);
  runicast_open(&conn->data_collection_conn, channels + 3, &data_collection_callback);
  runicast_open(&conn->actuation_conn, channels + 4, &actuation_callback);

  /* Initialize sensors forwarding structure */
  downward_table_sources = sensors;
  int i = 0;
  for(i=0;i<SENSORS;i++){
    linkaddr_copy(&downward_table_destinations[i], &linkaddr_null);
  }

  /* Init the QUEUE of collect messages for all nodes */
  list_init(collect_to_send);
  memb_init(&collect_mem);

  /* Tree construction (periodic) */
  if(conn->node_role == NODE_ROLE_CONTROLLER){
    /* QUEUE fo commands to send for controller */
    list_init(commands_to_send);
    memb_init(&actuation_mem);
    conn->tree_info.metric = 0; 

    /* Schedule the first beacon message flood */
    ctimer_set(&conn->tree_info.beacon_timer, BEACON_INTERVAL, tree_beacon_timer_cb, conn);
  }
  return true;
}
/*---------------------------------------------------------------------------*/

/**
 * @brief Turns off the protocol by shutting of the connections.
 * 
 * @param conn 
 */
void etc_close(struct etc_conn *conn)
{
  /* Turn off connections to ignore any incoming packet
   * and stop transmitting */
  broadcast_close(&conn->event_bc_conn);
  broadcast_close(&conn->tree_bc_conn);
  broadcast_close(&conn->actuation_opportunistic_conn);
  runicast_close(&conn->actuation_conn);
  runicast_close(&conn->data_collection_conn); 
}

/*---------------------------------------------------------------------------*/

/**
 * @brief Used by the app to share the most recent sensed value;
 *        ONLY USED BY SENSORS
 * 
 * @param value the read value
 * @param threshold the limit the node has for the "stability"
 */
void etc_update(uint32_t value, uint32_t threshold)
{
  /* Update local value and threshold, to be sent in case of event */
  sensor_event_data.value = value;
  sensor_event_data.threshold = threshold;
  
}

/*---------------------------------------------------------------------------*/

/**
 * @brief Start event dissemination (unless events are being suppressed to avoid
 *        contention).
 *        ONLY USED BY SENSORS 
 * 
 * @param conn 
 * @param value 
 * @param threshold 
 * @return Returns 0 if new events are currently being suppressed.
 */
int etc_trigger(struct etc_conn *conn, uint32_t value, uint32_t threshold)
{ 
  /* Check if node should suppress events */
  if(conn->suppress_internal_event){
    printf("Suppress new internal event\n");
    return 0;
  }

  /* Enable suppression of new internal events */
  ctimer_set(&conn->suppression_timer, SUPPRESSION_TIMEOUT_NEW, node_internal_event_cb, conn);
  conn->suppress_internal_event = true;

  /* Prepare event message */
  conn->event_source = linkaddr_node_addr;
  conn->event_seqn ++;

  /* Scheduling sending of EVENT message. This sensor will send it
   * without waiting for a delay. The receivers instead will wait 
   * for a short delay.
   */
  ctimer_set(&conn->event_prop_timer, EVENT_FORWARD_DELAY, send_event_delayed, conn);
  return 1;
}
/*---------------------------------------------------------------------------*/

/**
 * @brief Called by the controller to send commands to a given destination.
 *        Enqueues command messages to be later delivered. See "commands_to_send" queue.
 *        ONLY USED BY CONTROLLER
 * 
 * @param conn 
 * @param dest 
 * @param command 
 * @param threshold 
 * @return  Acutally is not checked by the app so returns 1 by default.
 */
int etc_command(struct etc_conn *conn, const linkaddr_t *dest,
            command_type_t command, uint32_t threshold)
{
  int delay;
  struct command_msg_list_t* command_element;
  command_element = memb_alloc(&actuation_mem);

  /* Saving into external data structure the last command message to be sent */
  command_element->actuation_command.command = command;
  linkaddr_copy(&command_element->actuation_command.dest, dest);
  command_element->actuation_command.event_seqn = conn->event_seqn;
  linkaddr_copy(&command_element->actuation_command.event_source, &conn->event_source);
  command_element->actuation_command.retransmitted = 0;
  command_element->actuation_command.treshold = threshold; 

  /* Add the command to the list of commands to be sent */
  list_add(commands_to_send, command_element);

  delay = ((int)linkaddr_node_addr.u16 + random_rand()) % (CLOCK_SECOND);
  ctimer_set(&conn->actuation_timer, delay, send_actuation_command, conn);
  return 1;
}


/*---------------------------------------------------------------------------*/
/*                              Beacon Handling                              */
/*---------------------------------------------------------------------------*/


/**
 * @brief Tree Beacon timer callback 
 * 
 * @param ptr 
 */
void tree_beacon_timer_cb(void* ptr)
{
  /* Common nodes and sink logic */
  struct etc_conn* conn = (struct etc_conn*)ptr;
  send_beacon(conn);

  /* Sink-only logic */
  if (conn->node_role == NODE_ROLE_CONTROLLER) {
    /* Rebuild the three from scratch after the beacon interval */
    conn->tree_info.beacon_seqn ++; 
    /* Schedule the next beacon message flood */
    ctimer_set(&conn->tree_info.beacon_timer, BEACON_INTERVAL, tree_beacon_timer_cb, conn);
  }
}

/**
 * @brief Send beacon using the current seqn and metric
 * 
 * @param conn 
 */
void send_beacon(struct etc_conn* conn)
{
  /* Prepare the beacon message */
  struct beacon_msg beacon = {
    .seqn = conn->tree_info.beacon_seqn, .metric = conn->tree_info.metric};

  /* Send the beacon message in broadcast */
  packetbuf_clear();
  packetbuf_copyfrom(&beacon, sizeof(beacon));
  printf("Tree building: sending beacon: seqn %d metric %d\n",
    beacon.seqn, beacon.metric);
  broadcast_send(&conn->tree_bc_conn);
}

/**
 * @brief Beacon receive callback
 * 
 * @param bc_conn 
 * @param sender 
 */
void tree_bc_recv(struct broadcast_conn *bc_conn, const linkaddr_t *sender)
{
  struct beacon_msg beacon;
  struct etc_conn* conn;
  int16_t rssi;

  /* Get the pointer to the overall structure from field bc */
  conn = (struct etc_conn*)(((uint8_t*)bc_conn) - 
    offsetof(struct etc_conn, tree_bc_conn));

  /* Check if the received broadcast packet looks legitimate */
  if (packetbuf_datalen() != sizeof(struct beacon_msg)) {
    printf("Tree building: broadcast of wrong size\n");
    return;
  }
  memcpy(&beacon, packetbuf_dataptr(), sizeof(struct beacon_msg));

  /* Read the RSSI of the *last* reception */
  rssi = packetbuf_attr(PACKETBUF_ATTR_RSSI);
  printf("Tree building: recv beacon from %02x:%02x seqn %u metric %u rssi %d\n", 
      sender->u8[0], sender->u8[1], 
      beacon.seqn, beacon.metric, rssi);

  /* 
   * Analyze the received beacon message based on RSSI, seqn, and metric.
   * Update (if needed) the node's current routing info (parent, metric, beacon_seqn).
   */
  if (rssi < RSSI_THRESHOLD || beacon.seqn < conn->tree_info.beacon_seqn)
    return; // The beacon is either too weak or too old, ignore it
  if (beacon.seqn == conn->tree_info.beacon_seqn) { // The beacon is not new, check the metric
    int parent_to_update = -1;
    // Is the beacon proposed parent better? 
    if ((((beacon.metric + 1 < conn->tree_info.metric) && (rssi >= conn->tree_info.rssi[0]))
        || ((beacon.metric + 1 <= conn->tree_info.metric) && (rssi > conn->tree_info.rssi[0])))
         && (linkaddr_cmp(&conn->tree_info.parents[0], sender) == 0)){
      parent_to_update = 0;
    }else if ((((beacon.metric + 1 < conn->tree_info.metric) && (rssi >= conn->tree_info.rssi[1]))
        || ((beacon.metric+1 <= conn->tree_info.metric) && (rssi > conn->tree_info.rssi[1])))
        && linkaddr_cmp(&conn->tree_info.parents[1], sender) == 0){
      // Is the proposted parent better than the reserve choice? 
      parent_to_update = 1;
    }else{

      /* Nodes must have a second candidate even if above conditions
       * did not apply. Check if node has a second parent, in negative
       * case set it to the proposed candidate on the only basis of RSSI.
       */
        if(linkaddr_cmp(&conn->tree_info.parents[1], &linkaddr_null)){
          parent_to_update = 1;
        }else if(rssi > conn->tree_info.rssi[1]){
          parent_to_update = 1;
        }
      if(parent_to_update == -1){
        printf("Tree building: Nothing to do\n");
        return; // Ignore beacon. Parent_to_update is -1.
      }
    }

    /* In case the new beacon parent is better than the saved best candidate, the old best becomes
     *  the second choice 
     */
    if(parent_to_update == 0){
      // Second candidate update with the first one values
      linkaddr_copy(&conn->tree_info.parents[1], &conn->tree_info.parents[0]);
      conn->tree_info.rssi[1] = conn->tree_info.rssi[0];

      // Updating first candidate
      linkaddr_copy(&conn->tree_info.parents[0], sender);
      conn->tree_info.metric = beacon.metric + 1;
      conn->tree_info.rssi[0] = rssi;
      conn->tree_info.beacon_seqn = beacon.seqn;
      printf("Tree building: new best parent %02x:%02x, my metric %d\n", 
        sender->u8[0], sender->u8[1], conn->tree_info.metric);
    }else{
      /* Updating second candidate only.
       * It will have same metric but better RSSI.
       */
      linkaddr_copy(&conn->tree_info.parents[1], sender);
      conn->tree_info.rssi[1] = rssi; 
      printf("Tree building: new second candidate %02x:%02x, my metric %d\n", 
        sender->u8[0], sender->u8[1], conn->tree_info.metric);
    }
  }else{
    
    /* Update second candidate with the first one values if 
     * the new first candidate is different from the actual one.
     * This avoids having the first and second cadidate equal.
     */  
    if(linkaddr_cmp(&conn->tree_info.parents[0], sender) == 0){
      linkaddr_copy(&conn->tree_info.parents[1], &conn->tree_info.parents[0]);
      conn->tree_info.rssi[1] = conn->tree_info.rssi[0];
    }
    
    // Update the first candidate
    linkaddr_copy(&conn->tree_info.parents[0], sender);
    conn->tree_info.metric = beacon.metric + 1;
    conn->tree_info.beacon_seqn = beacon.seqn;
    conn->tree_info.rssi[0] = rssi;
    printf("Tree building: new parent %02x:%02x, my metric %d\n", 
      sender->u8[0], sender->u8[1], conn->tree_info.metric);
  }
  /* Schedule beacon propagation */
  ctimer_set(&conn->tree_info.beacon_timer, BEACON_FORWARD_DELAY, tree_beacon_timer_cb, conn);
}
/*---------------------------------------------------------------------------*/
/*                               Event Handling                              */
/*---------------------------------------------------------------------------*/

/**
 * @brief Disable suppression of internal events
 * 
 * @param ptr 
 */
void node_internal_event_cb(void *ptr){
  struct etc_conn* conn = (struct etc_conn*)ptr; 
  conn->suppress_internal_event = false;
};

/**
 * @brief Disable suppression of propagation of events
 * 
 * @param ptr 
 */
void node_propagation_event_cb(void * ptr){
  struct etc_conn* conn = (struct etc_conn*)ptr; 
  conn->suppress_propagation = false;
}

/**
 * @brief Enables the EVENT flood by sending event in broadcast. Schedules
 *        the caller to send the collect message after a while.
 * @param ptr 
 */
void send_event_delayed(void* ptr){
  struct etc_conn* conn = (struct etc_conn*)ptr; 

  /* Sending event */
  send_event(conn);
    
  /* Scheduling sending of COLLECT message */
  ctimer_set(&conn->datacollection_timer, COLLECT_START_DELAY, data_collection_trigger, conn);
}

/**
 * @brief Send event in broadcast
 * 
 * @param conn 
 */
void send_event(void* ptr){
  struct etc_conn* conn = (struct etc_conn*)ptr;
  struct event_msg_t event_message = {.event_seqn = conn->event_seqn, .event_source = conn->event_source};
  packetbuf_clear();
  packetbuf_copyfrom(&event_message, sizeof(event_message));
  broadcast_send(&conn->event_bc_conn);
}


/**
 * @brief Callback for event reception
 * 
 * @param bc_conn 
 * @param sender 
 */
void event_bc_recv(struct broadcast_conn *bc_conn, const linkaddr_t *sender){

  struct etc_conn* conn;
  struct event_msg_t event;

  /* Get the pointer to the overall structure etc_conn from its field bc */
  conn = (struct etc_conn*)(((uint8_t*)bc_conn) - 
    offsetof(struct etc_conn, event_bc_conn));

  /* Propagation suppress enabled */
  if(conn->suppress_propagation){
    printf("Event flood DBG: suppress at node %02x:%02x\n",
      linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1]);
    return;
  }

  /* Saving event information */
  memcpy(&event, packetbuf_dataptr(), sizeof(struct event_msg_t));
  conn->event_source = event.event_source;
  conn->event_seqn = event.event_seqn;

  /* Controller should call app logic */
  if(conn->node_role == NODE_ROLE_CONTROLLER){
    conn->callbacks->ev_cb(&event.event_source, event.event_seqn);
    return;
  }

  /* Sensors: scheduling sending of COLLECT message */
  if(conn->node_role == NODE_ROLE_SENSOR_ACTUATOR){
    ctimer_set(&conn->datacollection_timer, COLLECT_TRIGGER , data_collection_trigger, conn);
  }

  /* Enable timers:
   * - suppress internal events
   * - disable propagation 
   */
  ctimer_set(&conn->suppression_prop_timer, SUPPRESSION_TIMEOUT_PROP, node_propagation_event_cb, conn);
  conn->suppress_propagation = true;
  ctimer_set(&conn->suppression_timer, SUPPRESSION_TIMEOUT_NEW, node_internal_event_cb, conn);
  conn->suppress_internal_event = true;

  /* Send the event in broadcast */
  send_event(conn);
  printf("Event [%d - %02x:%02x] propagation at node %02x:%02x\n",
      event.event_seqn, event.event_source.u8[0], event.event_source.u8[1], linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1]);

}

/**
 * @brief Sent callback for EVENT messages. Resend the event in case of collision.
 * 
 * @param bc_conn 
 * @param status 
 * @param num_tx 
 */
void event_bc_sent(struct broadcast_conn *bc_conn, int status, int num_tx){
  struct etc_conn* conn;
  /* Get the pointer to the overall structure etc_conn from its field bc */
  conn = (struct etc_conn*)(((uint8_t*)bc_conn) - 
    offsetof(struct etc_conn, event_bc_conn));

  if(status != MAC_TX_OK){
    /* Resend later */
    printf("Resend event\n");
    ctimer_set(&conn->event_prop_timer, EVENT_FORWARD_DELAY, send_event, conn);
  }
}

/*---------------------------------------------------------------------------*/
/*                               Data Handling                               */
/*---------------------------------------------------------------------------*/

/**
 * @brief The triggered sensor prepares to send his own collect message.
 *        Enqueue the collect message in the queue of collect messages
 *        to be delivered. See "List collect_to_send"
 * @param ptr 
 */
void data_collection_trigger(void* ptr)
{
  struct etc_conn* conn = (struct etc_conn*)ptr; 
  if((conn->node_role == NODE_ROLE_FORWARDER) || (conn->node_role == NODE_ROLE_CONTROLLER)){
    printf("--ERROR: Data collection triggered not a sensor\n");
    return;
  }
  printf("Triggered sensor [%02x:%02x] \n", linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1]);

  /* Allocating list element */
  struct collect_msg_list_t* collect_element;
  collect_element = memb_alloc(&collect_mem);

  /* Saving this collect message in the queue of messages to be sent by this node */
  collect_element->collect_message.event_seqn = conn->event_seqn;
  linkaddr_copy(&collect_element->collect_message.event_source, &conn->event_source);
  collect_element->collect_message.retransmitted = 0;
  collect_element->collect_message.s_data.threshold = sensor_event_data.threshold;
  collect_element->collect_message.s_data.value = sensor_event_data.value;
  linkaddr_copy(&collect_element->collect_message.s_data.sensor_addr, &linkaddr_node_addr);
  list_add(collect_to_send, collect_element);

  /* Scheduling the actual send */
  ctimer_set(&data_transmission_timer, DATA_SENSOR_OFFSET, data_collection_send, conn);
  return;
}

/**
 * @brief Send a data collection message taken from the "collect_to_send" queue
 * 
 * @param ptr casted to etc_conn
 */
void data_collection_send(void* ptr){
  struct etc_conn* conn = (struct etc_conn*)ptr;
  struct collect_msg_list_t* head;
  int ret, ret2;
  bool try_second_parent = false;

  // Pick a collect message to be sent 
  head = list_head(collect_to_send);
  if(head == NULL){
    printf("All collect messages sent by this node\n");
    return;
  }

  // Check best parent 
  if (linkaddr_cmp(&conn->tree_info.parents[0], &linkaddr_null))
    // The node is still disconnected 
    return; 

  packetbuf_clear();
  packetbuf_copyfrom(&head->collect_message, sizeof(head->collect_message));

  // Has the collect message been already retransmitted by this node? 
  if(head->collect_message.retransmitted > 0 && head->collect_message.retransmitted < MAX_DATA_RETRANSMISSIONS){
    printf("Collection RESEND [%02x:%02x - %d] of [%02x:%02x]\n", head->collect_message.event_source.u8[0], head->collect_message.event_source.u8[1], head->collect_message.event_seqn, head->collect_message.s_data.sensor_addr.u8[0], head->collect_message.s_data.sensor_addr.u8[1]);
    if(head->collect_message.retransmitted >= (MAX_DATA_RETRANSMISSIONS/2)){
      try_second_parent = true;
    }
  }else if(head->collect_message.retransmitted >= MAX_DATA_RETRANSMISSIONS){
    // Max number of retransmissions reached 
    printf("Max retransmissions\n");
    list_pop(collect_to_send);
    memb_free(&collect_mem, head);
  }

  if(!try_second_parent){
    /* Send the collect message to the node's parent in unicast.
     * Both parents could be employed to perform the send.
     */
    ret = runicast_send(&conn->data_collection_conn, &conn->tree_info.parents[0], MAX_DATA_RETRANSMISSIONS); 
    if (ret == 0){
      ret = runicast_send(&conn->data_collection_conn, &conn->tree_info.parents[1], MAX_DATA_RETRANSMISSIONS); 
      if(ret == 0){
        // Both send couldn't start. Scheduling retransmission 
        printf("Data collection failed to start from this node. Other attempts later\n");
        ctimer_set(&data_retransmission_timer, DATA_RETRANSMIT_OFFSET, data_collection_resend, conn);
        return;
      }else{
        printf("Collection forwoard: sensor sending [%02x:%02x - %d] of [%02x:%02x] to second parent [%02x:%2x]\n", head->collect_message.event_source.u8[0], head->collect_message.event_source.u8[1], head->collect_message.event_seqn, head->collect_message.s_data.sensor_addr.u8[0], head->collect_message.s_data.sensor_addr.u8[1], conn->tree_info.parents[1].u8[0], conn->tree_info.parents[1].u8[1]);
      } 
    }else{
      printf("Collection forwoard: sensor sending [%02x:%02x - %d] of [%02x:%02x] to first parent [%02x:%02x]\n", head->collect_message.event_source.u8[0], head->collect_message.event_source.u8[1], head->collect_message.event_seqn, head->collect_message.s_data.sensor_addr.u8[0], head->collect_message.s_data.sensor_addr.u8[1], conn->tree_info.parents[0].u8[0], conn->tree_info.parents[0].u8[1]);
    }
  }else{
    // The node already retransmitted some times. Switch to use the second parent only 
    head->collect_message.retransmitted++;
    ret2 = runicast_send(&conn->data_collection_conn, &conn->tree_info.parents[1], MAX_DATA_RETRANSMISSIONS);
    if (ret2 == 0){
      ctimer_set(&data_retransmission_timer, DATA_RETRANSMIT_OFFSET, data_collection_resend, conn);
    }else{
      printf("Collection forwoard: sensor sending [%02x:%02x - %d] of [%02x:%02x] to second parent [%02x:%2x]\n", head->collect_message.event_source.u8[0], head->collect_message.event_source.u8[1], head->collect_message.event_seqn, head->collect_message.s_data.sensor_addr.u8[0], head->collect_message.s_data.sensor_addr.u8[1], conn->tree_info.parents[1].u8[0], conn->tree_info.parents[1].u8[1]);
    }
  }

  /* Scheduling recursive call */
  ctimer_set(&data_transmission_timer, DATA_TRANSMIT_OFFSET, data_collection_send, conn);
}

/**
 * @brief Callback for COLLECT message reception. Non sink nodes forwards
 *        the message by pushing it in the proper queue ("collect_to_send")
 *        They then schedules the actual send after some delay. 
 *        The sink calls the app logic.
 * 
 * @param c 
 * @param from 
 * @param seqno 
 */
void data_collection_uc_recv (struct runicast_conn *c, const linkaddr_t *from, uint8_t seqno)
{
  int idx = 0;
  struct etc_conn * conn;
  struct collect_msg_t collect_msg;
  
  /* Get the pointer to the overall structure my_collect_conn from its field data_collection_conn */
  conn = (struct etc_conn*)(((uint8_t*)c) - 
    offsetof(struct etc_conn, data_collection_conn));

  /* Check if the received unicast message looks legitimate */
  if (packetbuf_datalen() < sizeof(struct collect_msg_t)) {
    printf("Data collection: too short unicast packet %d\n", packetbuf_datalen());
    return;
  }
  memcpy(&collect_msg, packetbuf_dataptr(), sizeof(collect_msg));

  /* Loop detection - 
   *
   * Why? The node has two parents, the first one respects the motonicity
   * of the metric therefore is part of a consistent tree; the second node
   * is just a different node which might be chosen if it has the best signal 
   * regardless of the metric. This could lead to loops in case this node has 
   * scheduled trasmission trough the second node because the first one was 
   * not available.
   */
  if(linkaddr_cmp(&linkaddr_node_addr, &collect_msg.s_data.sensor_addr)){
    printf("Data collection DROP: loop detected. [%02x:%02x - %d] from [%02x:%02x]\n", collect_msg.event_source.u8[0], collect_msg.event_source.u8[1], collect_msg.event_seqn, from->u8[0], from->u8[1]);
    return;
  }
  printf("Collection packet [%02x:%02x - %d] of [%02x:%02x] received from [%02x:%02x]\n", collect_msg.event_source.u8[0], collect_msg.event_source.u8[1], collect_msg.event_seqn, collect_msg.s_data.sensor_addr.u8[0], collect_msg.s_data.sensor_addr.u8[1], from->u8[0], from->u8[1]);

  // Simple duplicate filter
  if((last_collect.event_seqn == collect_msg.event_seqn) && linkaddr_cmp(&last_collect.event_source, &collect_msg.event_source)
    && linkaddr_cmp(&last_collect.s_data.sensor_addr, &collect_msg.s_data.sensor_addr)){
      // Node already managed such message 
      printf("Node already managed collect [%02x:%02x - %d] of [%02x:%02x]\n", collect_msg.event_source.u8[0], collect_msg.event_source.u8[1], collect_msg.event_seqn, collect_msg.s_data.sensor_addr.u8[0], collect_msg.s_data.sensor_addr.u8[1]);
      return;
    }

  // Updating downward routing table 
  while(linkaddr_cmp(&downward_table_sources[idx], &collect_msg.s_data.sensor_addr) == 0){idx++;}
  linkaddr_copy(&downward_table_destinations[idx], from);

  printf("Table update: [%02x:%02x] -> [%02x:%02x]\n", 
        downward_table_sources[idx].u8[0], downward_table_sources[idx].u8[1], 
        from->u8[0], from->u8[1]);

  // This node saves externally the last collect message received 
  memcpy(&last_collect, &collect_msg, sizeof(collect_msg));

  /* The destination has been reached, no more forwarding is needed.
   * Calling app recv callback.
   */
  if (conn->node_role == NODE_ROLE_CONTROLLER) { 
    conn->callbacks->recv_cb(&collect_msg.event_source, collect_msg.event_seqn, &collect_msg.s_data.sensor_addr, collect_msg.s_data.value, collect_msg.s_data.threshold);
  } else {
    /* Non-sink node acting as a forwarder. 
     * Add a message to be delivered in case is not already in the queue 
     */
    if(check_collect_message_queue(&collect_msg)){
      printf("Collect [%02x:%02x - %d] of [%02x:%02x] already in queue to be sent\n", collect_msg.event_source.u8[0], collect_msg.event_source.u8[1], collect_msg.event_seqn, collect_msg.event_source.u8[0], collect_msg.event_source.u8[1]);
      return;
    }
    struct collect_msg_list_t* collect_element;
    collect_element = memb_alloc(&collect_mem);
    collect_element->collect_message = collect_msg;
    list_add(collect_to_send, collect_element);

    if (linkaddr_cmp(&conn->tree_info.parents[0], &linkaddr_null)) { 
      printf("Collection: No parent -- source: %02x:%02x, seqn: %u\n", collect_msg.event_source.u8[0], collect_msg.event_source.u8[1], collect_msg.event_seqn);
      return;
    }

    /* Scheduling the actual send */
    ctimer_set(&data_transmission_timer, DATA_SENSOR_OFFSET, data_collection_send, conn);
  }
}

/**
 * @brief Callback for data collection successful transmission. Removes the
 *        successful sent packet from the "collect_to_send" queue.
 * 
 * @param c 
 * @param to 
 * @param retransmissions 
 */
void sent_data_rc(struct runicast_conn *c, const linkaddr_t *to, uint8_t retransmissions){
  printf("Collection message sent to %d.%d, retransmissions %d\n",
	 to->u8[0], to->u8[1], retransmissions);

  struct etc_conn* conn;
  // Get the pointer to the overall structure 
  conn = (struct etc_conn*)(((uint8_t*)c) - 
    offsetof(struct etc_conn, data_collection_conn));

  struct collect_msg_list_t* head;
  head = list_head(collect_to_send);
  if(head == NULL){
    // Empty list check
    return;
  }

  // Collect message sent, remove it 
  list_pop(collect_to_send);
  memb_free(&collect_mem, head);

  // Call in case there are still messages to be delivered 
  ctimer_set(&data_transmission_timer, DATA_RETRANSMIT_OFFSET, data_collection_send, conn);
}

/**
 * @brief Callback for data collection runicast send. Called on timeout. Reschedules another send attempt
 * 
 * @param c 
 * @param to 
 * @param retransmissions 
 */
void sent_data_timedout(struct runicast_conn *c, const linkaddr_t *to, uint8_t retransmissions){
  printf("Collection FAIL: timed out when sending to %d.%d, retransmissions %d\n",
    to->u8[0], to->u8[1], retransmissions);
  
  struct etc_conn* conn;
  // Get the pointer to the overall structure 
  conn = (struct etc_conn*)(((uint8_t*)c) - 
    offsetof(struct etc_conn, data_collection_conn));

  // Schedule another attempt
  ctimer_set(&data_retransmission_timer, DATA_RETRANSMIT_OFFSET, data_collection_resend, conn);
}

/**
 * @brief Should be called to retransmit sensor data to controller after failures.
 *        Typically resends packet via second parent.
 * 
 * @param ptr casted to etc_conn
 */
static void data_collection_resend(void *ptr){
  struct etc_conn* conn = (struct etc_conn*)ptr;
  struct collect_msg_list_t* head;

  head = list_head(collect_to_send);
  if(head == NULL){
    // Empty list check 
    printf("All collect messages resent\n");
    return;
  }
  head->collect_message.retransmitted++;
  // Call in case there are still messages to be delivered 
  ctimer_set(&data_transmission_timer, DATA_RETRANSMIT_OFFSET, data_collection_send, conn);
}

/*---------------------------------------------------------------------------*/
/*                               Command Handling                            */
/*---------------------------------------------------------------------------*/

/**
 * @brief Function to send the COMMAND messages from the "commands_to_send" queue. 
 *        It calls itself recursively in case there are pending messages to be delivered.
 * 
 * @param ptr casted to "etc_conn"
 */
void send_actuation_command(void* ptr){
  struct etc_conn *conn = (struct etc_conn*) ptr;
  struct command_msg_list_t *head;
  int ret;
  int idx;
  
  // Pick a command message from the commands to be delivered list 
  head = list_head(commands_to_send);
  if(head == NULL){
    // Empty list check 
    printf("All actuation commands sent\n");
    return;
  }

  // There is a command message to be sent 
  packetbuf_clear();
  packetbuf_copyfrom(&head->actuation_command, sizeof(head->actuation_command));
  idx = find_sensor_index(&head->actuation_command.dest);
  
  // Has the command message already been retransmitted by this node? 
  if(head->actuation_command.retransmitted >0 && head->actuation_command.retransmitted < MAX_COMMAND_RETRANSMISSIONS){
    /* Since the runicast primitive failed to send to the parent of downward_table
     * the resend mechanism, after some attemps, is the Opportunistic forwarding
     */
    if(head->actuation_command.retransmitted >= (MAX_COMMAND_RETRANSMISSIONS/2)){
      printf("--COMMAND opportunistic resend of [%02x:%02x - %d] \n", head->actuation_command.dest.u8[0], head->actuation_command.dest.u8[1], head->actuation_command.event_seqn);     
      memcpy(&opportunistic_actuation_command, &head->actuation_command, sizeof(head->actuation_command));
      ctimer_set(&conn->actuation_opp_send_timer, ACTUATION_MULTIPLE_SEND_OFFSET, send_actuation_opportunistic, conn);
      return;
    }
    printf("--COMMAND RESEND to [%02x:%02x]\n", downward_table_destinations[idx].u8[0], downward_table_destinations[idx].u8[1]);     

  }else if(head->actuation_command.retransmitted >= MAX_COMMAND_RETRANSMISSIONS){
    // Max retransmission reached, stop sending such message
    printf("Max retransmission of command message\n");
    list_pop(commands_to_send);
    memb_free(&actuation_mem, head);
  }

  ret = runicast_send(&conn->actuation_conn, &downward_table_destinations[idx], MAX_COMMAND_RETRANSMISSIONS); 
  printf("Node actuation forwoard: sending [%02x:%02x - %d] to [%02x:%02x]\n", head->actuation_command.dest.u8[0], head->actuation_command.dest.u8[1], head->actuation_command.event_seqn, downward_table_destinations[idx].u8[0], downward_table_destinations[idx].u8[1]);
  if (ret == 0){
    printf("Node actuation forwoard: FAIL sending [%02x:%02x - %d] to [%02x:%02x]. Scheduling another attempt\n", head->actuation_command.dest.u8[0], head->actuation_command.dest.u8[1], head->actuation_command.event_seqn, downward_table_destinations[idx].u8[0], downward_table_destinations[idx].u8[1]);
    ctimer_set(&actuation_retransmission_timer, ACTUATION_RETRANSMIT_OFFSET, actuation_resend, conn);
    return;
  }
}

/**
 * @brief Callback for command message reception. Commands might be forwoarded or
 *        managed by the sensor. It depends on the final destination contained in 
 *        the packet. In case the proper sensor/actuator is reached the app com_cb 
 *        callback is called.
 * 
 * @param uc_conn 
 * @param from 
 */
void actuation_command_uc_recv(struct runicast_conn *c, const linkaddr_t *from, uint8_t seqno){
  struct etc_conn *conn;
  struct command_msg_t command_msg;
  struct command_msg_list_t* command_element;

  // Get the pointer to the overall structure etc_conn from actuation_conn 
  conn = (struct etc_conn*)(((uint8_t*)c) - 
    offsetof(struct etc_conn, actuation_conn));

  // Check if the received unicast message looks legitimate 
  if (packetbuf_datalen() < sizeof(struct command_msg_t)) {
    printf("--COMMAND: too short unicast packet %d\n", packetbuf_datalen());
    return;
  }
  memcpy(&command_msg, packetbuf_dataptr(), sizeof(command_msg));

  printf("CONN: %d, message %d\n", conn->event_seqn, command_msg.event_seqn);
  // Making sure node agrees with the vision of the controller 
  conn->event_seqn = command_msg.event_seqn;
  linkaddr_copy(&conn->event_source, &command_msg.event_source);


  /* Simple duplicate check
   *  
   * Node could have already succesfully sent/received this actuation command but it may
   * be triggered again because the sender did not receive ack.
   */
  if(last_actuation_command.event_seqn != command_msg.event_seqn || !linkaddr_cmp(&last_actuation_command.event_source, &command_msg.event_source) || !(linkaddr_cmp(&last_actuation_command.dest, &command_msg.dest))){
    // Check if the node is the destination 
    if(linkaddr_cmp(&command_msg.dest, &linkaddr_node_addr) == 0){
      
      if(check_command_message_queue(&command_msg)){
        printf("Command [%02x:%02x - %d] to [%02x:%02x] already in queue to be sent\n", command_msg.event_source.u8[0], command_msg.event_source.u8[1], command_msg.event_seqn, command_msg.dest.u8[0], command_msg.dest.u8[1]);
        return;
      }
      command_element = memb_alloc(&actuation_mem);
      command_element->actuation_command = command_msg;

      // Add the command to the list of commands to be sent 
      list_add(commands_to_send, command_element);
      ctimer_set(&actuation_multiple_send_timer, ACTUATION_MULTIPLE_SEND_OFFSET, send_actuation_command, conn);
    }else{
      // This is the final destination. Com_cb is called 
      printf("--COMMAND sensor [%02x:%02x] reached\n", linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1]);
      conn->callbacks->com_cb(&command_msg.event_source, command_msg.event_seqn, command_msg.command, command_msg.treshold);
      
      /* Avoids to retransmit if ack is not get.
       * Since this node is the final destination it avoids
       * to call multiple times the com_cb.
       */
      memcpy(&last_actuation_command, &command_msg, sizeof(command_msg));
    }
  }else{
    printf("--COMMAND: node already successfully managed [%d - %02x:%02x]. Dropping...\n", conn->event_seqn, command_msg.dest.u8[0], command_msg.dest.u8[1]);
  }
}

/**
 * @brief Callback for actuation commands opportunistic reception.
 *        Node will forwoard the command in broadcast.
 * 
 * @param c 
 * @param sender 
 */
void actuation_opp_bc_recv(struct broadcast_conn *c, const linkaddr_t *sender){
    struct etc_conn *conn;
    struct command_msg_t command_msg;

    // Get the pointer to the overall structure etc_conn from actuation_opportunistic_conn 
    conn = (struct etc_conn*)(((uint8_t*)c) - 
      offsetof(struct etc_conn, actuation_opportunistic_conn));

    // Check if the received message looks legitimate 
    if (packetbuf_datalen() < sizeof(struct command_msg_t)) {
      printf("--COMMAND opportunistic: too short packet %d\n", packetbuf_datalen());
      return;
    }
    memcpy(&command_msg, packetbuf_dataptr(), sizeof(command_msg));
    printf("--COMMAND opportunistic: reception of command for event [%02x:%02x - %d] to [%02x:%02x]\n", command_msg.event_source.u8[0], command_msg.event_source.u8[1], command_msg.event_seqn ,command_msg.dest.u8[0], command_msg.dest.u8[1]);

    // Is this event already managed? Simple duplicate filter 
    if((last_opportunistic_actuation_command.event_seqn == command_msg.event_seqn) && (linkaddr_cmp(&last_opportunistic_actuation_command.event_source, &command_msg.event_source))
      && (last_opportunistic_actuation_command.command == command_msg.command)){
        printf("--COMMAND opportunistic: node already successfully managed [%02x:%02x - %d - CMD: [%d]]. Dropping...\n", command_msg.event_source.u8[0], command_msg.event_source.u8[1], command_msg.event_seqn, command_msg.command);
        return;
    }

    // Has the destination been reached? 
    if(linkaddr_cmp(&command_msg.dest, &linkaddr_node_addr) == 0){
      /* Node is not the destination of the command, forwarding 
       * Check for suppression of commands */ 
      if(conn->suppress_opportunistic){
        printf("Suppress opportunistic resend of commands\n");
        return;
      }

      // Enable suppressing of commands 
      ctimer_set(&actuation_opp_suppress_timer, ACTUATION_OPPORTUNISTIC_OFFSET, opportunistic_timer_cb, conn);
      conn->suppress_opportunistic = true;

      // Scheduling the send
      memcpy(&opportunistic_actuation_command, packetbuf_dataptr(), sizeof(opportunistic_actuation_command));
      ctimer_set(&conn->actuation_opp_send_timer, ACTUATION_MULTIPLE_SEND_OFFSET, send_actuation_opportunistic, conn);
    }else{
      printf("--COMMAND opporunistic: sensor [%02x:%02x] reached\n", linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1]);
      conn->callbacks->com_cb(&command_msg.event_source, command_msg.event_seqn, command_msg.command, command_msg.treshold);
    }
    // Saving this command message as the last managed by the sensor 
    memcpy(&last_opportunistic_actuation_command, &command_msg, sizeof(command_msg));
}

/**
 * @brief Sends the command message in broadcast (opportunistic forwarding)
 * 
 * @param ptr casted to etc_conn
 */
void send_actuation_opportunistic(void *ptr){
  struct etc_conn *conn = (struct etc_conn*) ptr;
  packetbuf_clear();
  packetbuf_copyfrom(&opportunistic_actuation_command, sizeof(opportunistic_actuation_command));
  broadcast_send(&conn->actuation_opportunistic_conn);
  printf("--COMMAND opportunistic: sending command to [%02x:%02x]\n", opportunistic_actuation_command.dest.u8[0], opportunistic_actuation_command.dest.u8[1]);
}

/**
 * @brief Reset the flag for enabling opportunistic command forwoarding
 * 
 * @param ptr 
 */
void opportunistic_timer_cb(void *ptr){
  struct etc_conn* conn = (struct etc_conn*)ptr; 
  conn->suppress_opportunistic = false;
};

/**
 * @brief Callback for correct send of COMMAND messages.
 * 
 * @param c 
 * @param to 
 * @param retransmissions 
 */
void sent_actuation_rc(struct runicast_conn *c, const linkaddr_t *to, uint8_t retransmissions){
  printf("--COMMAND message sent to %d.%d, retransmissions %d\n",
	 to->u8[0], to->u8[1], retransmissions);

  struct etc_conn* conn;
  // Get the pointer to the overall structure 
  conn = (struct etc_conn*)(((uint8_t*)c) - 
    offsetof(struct etc_conn, actuation_conn));

  struct command_msg_list_t* head;
  head = list_head(commands_to_send);
  if(head == NULL){
    // Empty list check 
    return;
  }

  // Command message sent successfully, remove it 
  list_pop(commands_to_send);
  memb_free(&actuation_mem, head);

  // Schedule another attempt till there are actuation commands to be delivered 
  ctimer_set(&actuation_retransmission_timer, ACTUATION_RETRANSMIT_OFFSET, send_actuation_command, conn);
}


/**
 * @brief Callback for timeout of COMMAND messages.
 * 
 * @param c 
 * @param to 
 * @param retransmissions 
 */
void sent_actuation_timedout(struct runicast_conn *c, const linkaddr_t *to, uint8_t retransmissions){
  printf("--COMMAND FAIL: timed out when sending to [%02x:%02x - %d], retransmissions %d\n",
        to->u8[0], to->u8[1], last_actuation_command.event_seqn, retransmissions);

  struct etc_conn* conn;
  /* Get the pointer to the overall structure */
  conn = (struct etc_conn*)(((uint8_t*)c) - 
    offsetof(struct etc_conn, actuation_conn));
  /* Schedule another attempt */
  ctimer_set(&actuation_retransmission_timer, ACTUATION_RETRANSMIT_OFFSET, actuation_resend, conn);
}

/**
 * @brief Should be used to resend COMMAND messages after some time. Use it
 *        in combination of "actuation_retransmission_timer".
 * 
 * @param ptr casted to etc_conn
 */
void actuation_resend(void *ptr){
  struct etc_conn* conn = (struct etc_conn*)ptr;
  struct command_msg_list_t* head;

  head = list_head(commands_to_send);
  if(head == NULL){
    // Empty list check 
    return;
  }
  head->actuation_command.retransmitted++;
  send_actuation_command(conn);
  return;    
}
/*---------------------------------------------------------------------------*/
/* Utilities */
/*---------------------------------------------------------------------------*/


/**
 * @brief Returns the source address index in the downward table
 * 
 * @param source 
 * @return int 
 */
int find_sensor_index(const linkaddr_t *source){
  int idx = 0;
  while(linkaddr_cmp(source, &downward_table_sources[idx]) == 0){idx++;}
  return idx;
}

/**
 * @brief Utility function to check if the message is in the "collect_to_send" queue
 * 
 * @param message collect message
 * @return true if the message is in the queue
 * @return false 
 */
bool check_collect_message_queue(struct collect_msg_t* message){
  struct collect_msg_list_t* head;
  for(head = list_head(collect_to_send); head != NULL; head = list_item_next(head)) {
    if(linkaddr_cmp(&head->collect_message.event_source, &message->event_source)
      && (head->collect_message.event_seqn == message->event_seqn)
      && linkaddr_cmp(&head->collect_message.s_data.sensor_addr, &message->s_data.sensor_addr))
      return true;
  }
  return false;
}

/**
 * @brief Utility function to check if the message is in the "commands_to_send" queue
 * 
 * @param message command message
 * @return true 
 * @return false 
 */
bool check_command_message_queue(struct command_msg_t* message){
  struct command_msg_list_t * head;
  for(head = list_head(commands_to_send); head != NULL; head = list_item_next(head)) {
    if(linkaddr_cmp(&head->actuation_command.event_source, &message->event_source)
      && (head->actuation_command.event_seqn == message->event_seqn)
      && linkaddr_cmp(&head->actuation_command.dest, &message->dest))
      return true;
  }
  return false;
}