/*
 * =====================================================================================
 *
 *       Filename:  net.c
 *
 *    Description:  This file contains general pupose Networking routines
 *
 *        Version:  1.0
 *        Created:  Wednesday 18 September 2019 08:36:50  IST
 *       Revision:  1.0
 *       Compiler:  gcc
 *
 *         Author:  Er. Abhishek Sagar, Networking Developer (AS), sachinites@gmail.com
 *        Company:  Brocade Communications(Jul 2012- Mar 2016), Current : Juniper Networks(Apr 2017 - Present)
 *        
 *        This file is part of the NetworkGraph distribution (https://github.com/sachinites).
 *        Copyright (c) 2017 Abhishek Sagar.
 *        This program is free software: you can redistribute it and/or modify
 *        it under the terms of the GNU General Public License as published by  
 *        the Free Software Foundation, version 3.
 *
 *        This program is distributed in the hope that it will be useful, but 
 *        WITHOUT ANY WARRANTY; without even the implied warranty of 
 *        MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU 
 *        General Public License for more details.
 *
 *        You should have received a copy of the GNU General Public License 
 *        along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * =====================================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include "graph.h"
#include "utils.h"
#include "tcpconst.h"
#include "notif.h"
#include "LinuxMemoryManager/uapi_mm.h"

/*Just some Random number generator*/
static uint32_t
hash_code(void *ptr, uint32_t size){
    uint32_t value=0, i =0;
    char *str = (char*)ptr;
    while(i < size)
    {
        value += *str;
        value*=97;
        str++;
        i++;
    }
    return value;
}


/*Heuristics, Assign a unique mac address to interface*/
void
interface_assign_mac_address(interface_t *interface){

    node_t *node = interface->att_node;
    
    if(!node)
        return;

    uint32_t hash_code_val = 0;
    hash_code_val = hash_code(node->node_name, NODE_NAME_SIZE);
    hash_code_val *= hash_code(interface->if_name, IF_NAME_SIZE);
    memset(IF_MAC(interface), 0, sizeof(IF_MAC(interface)));
    memcpy(IF_MAC(interface), (char *)&hash_code_val, sizeof(uint32_t));
}

typedef struct l3_route_ l3_route_t;

extern void
rt_table_add_direct_route(rt_table_t *rt_table, const char *ip_addr, char mask); 

bool node_set_loopback_address(node_t *node, const char *ip_addr){

    assert(ip_addr);

    node->node_nw_prop.is_lb_addr_config = true;
    strncpy((char *)NODE_LO_ADDR(node), ip_addr, 16);
    NODE_LO_ADDR(node)[15] = '\0';

    /*Add it as direct route in routing table*/
    rt_table_add_direct_route(NODE_RT_TABLE(node), ip_addr, 32);     
    return true;
}

bool node_set_intf_ip_address(node_t *node, const char *local_if, 
                                const char *ip_addr, char mask) {

    interface_t *interface = node_get_intf_by_name(node, local_if);
    if(!interface) assert(0);

    strncpy((char *)IF_IP(interface), ip_addr, 16);
    IF_IP(interface)[15] = '\0';
    interface->intf_nw_props.mask = mask; 
    interface->intf_nw_props.is_ipadd_config = true;
    rt_table_add_direct_route(NODE_RT_TABLE(node), ip_addr, mask);
    return true;
}

bool node_unset_intf_ip_address(node_t *node, const char *local_if){

    (unused) node;
    (unused) local_if;
    return true;
}

void dump_node_nw_props(node_t *node){

    printf("\nNode Name = %s UDP Port # : %u\n",
        node->node_name, node->udp_port_number);

    printf("\t node flags : %u", node->node_nw_prop.flags);

    if(node->node_nw_prop.is_lb_addr_config){
        printf("\t  lo addr : %s/32", NODE_LO_ADDR(node));
    }

    printf("\n");
}

void dump_intf_props(interface_t *interface){

    dump_interface(interface);

    printf("\t If Status : %s\n", IF_IS_UP(interface) ? "UP" : "DOWN");

    if(interface->intf_nw_props.is_ipadd_config){
        printf("\t IP Addr = %s/%u", IF_IP(interface), interface->intf_nw_props.mask);
        printf("\t MAC : %02x:%02x:%02x:%02x:%02x:%02x\n", 
                IF_MAC(interface)[0], IF_MAC(interface)[1],
                IF_MAC(interface)[2], IF_MAC(interface)[3],
                IF_MAC(interface)[4], IF_MAC(interface)[5]);
    }
    else{
         printf("\t l2 mode = %s", intf_l2_mode_str(IF_L2_MODE(interface)));
         printf("\t vlan membership : ");
         int i = 0;
         for(; i < MAX_VLAN_MEMBERSHIP; i++){
            if(interface->intf_nw_props.vlans[i]){
                printf("%u  ", interface->intf_nw_props.vlans[i]);
            }
         }
         printf("\n");
    }
}

void dump_nw_graph(graph_t *graph, node_t *node1){

    node_t *node;
    glthread_t *curr;
    interface_t *interface;
    uint32_t i;
    
    printf("Topology Name = %s\n", graph->topology_name);
    
    if(!node1){
        ITERATE_GLTHREAD_BEGIN(&graph->node_list, curr){

            node = graph_glue_to_node(curr);
            dump_node_nw_props(node);
            for( i = 0; i < MAX_INTF_PER_NODE; i++){
                interface = node->intf[i];
                if(!interface) break;
                dump_intf_props(interface);
            }
        } ITERATE_GLTHREAD_END(&graph->node_list, curr);
    }
    else{
        dump_node_nw_props(node1);
        for( i = 0; i < MAX_INTF_PER_NODE; i++){
            interface = node1->intf[i];
            if(!interface) break;
            dump_intf_props(interface);
        }
    }
}

/*Returns the local interface of the node which is configured 
 * with subnet in which 'ip_addr' lies
 * */
interface_t *
node_get_matching_subnet_interface(node_t *node, char *ip_addr){

    uint32_t i = 0;
    interface_t *intf;

    unsigned char *intf_addr = NULL;
    char mask;
    unsigned char intf_subnet[16];
    unsigned char subnet2[16];

    for( ; i < MAX_INTF_PER_NODE; i++){
    
        intf = node->intf[i];
        if (!intf) return NULL;

        if (intf->intf_nw_props.is_ipadd_config == false)
            continue;
        
        intf_addr = IF_IP(intf);
        mask = intf->intf_nw_props.mask;

        memset(intf_subnet, 0 , 16);
        memset(subnet2, 0 , 16);
        apply_mask(intf_addr, mask, intf_subnet);
        apply_mask((unsigned char *)ip_addr, mask, subnet2);
        
        if (strncmp((char *)intf_subnet, (char *)subnet2, 16) == 0){
            return intf;
        }
    }
    return NULL;
}

bool 
is_same_subnet(unsigned char *ip_addr,
               char mask, 
               unsigned char *other_ip_addr){

    char intf_subnet[16];
    char subnet2[16];

    memset(intf_subnet, 0 , 16);
    memset(subnet2, 0 , 16);

    apply_mask(ip_addr, mask, (unsigned char*)intf_subnet);
    apply_mask(other_ip_addr, mask, (unsigned char*)subnet2);

    if (strncmp(intf_subnet, subnet2, 16) == 0){
        return true;
    }
    assert(0);
    return false;
}

/*Interface Vlan mgmt APIs*/

/*Should be Called only for interface operating in Access mode*/
uint32_t
get_access_intf_operating_vlan_id(interface_t *interface){

    if (IF_L2_MODE(interface) != ACCESS){
        assert(0);
    }

    return interface->intf_nw_props.vlans[0];
}


/*Should be Called only for interface operating in Trunk mode*/
bool
is_trunk_interface_vlan_enabled(interface_t *interface, 
                                uint32_t vlan_id){

    if (IF_L2_MODE(interface) != TRUNK){
        assert(0);
    }

    uint32_t i = 0;

    for( ; i < MAX_VLAN_MEMBERSHIP; i++){

        if(interface->intf_nw_props.vlans[i] == vlan_id)
            return true;
    }
    return false;
}

/*When pkt moves from top to down in TCP/IP stack, we would need
  room in the pkt buffer to attach more new headers. Below function
  simply shifts the pkt content present in the start of the pkt buffer
  towards right so that new room is created*/
char *
pkt_buffer_shift_right(char *pkt,
                                    uint32_t pkt_size, 
                                    uint32_t total_buffer_size){

    char *temp = NULL;
    bool need_temp_memory = false;

    if(pkt_size * 2 > (total_buffer_size - PKT_BUFFER_RIGHT_ROOM)){
        need_temp_memory = true;
    }
    
    if(need_temp_memory){
        temp = (char *)calloc(1, pkt_size);
        memcpy(temp, pkt, pkt_size);
        memset(pkt, 0, total_buffer_size);
        memcpy(pkt + (total_buffer_size - pkt_size - PKT_BUFFER_RIGHT_ROOM), 
            temp, pkt_size);
        free(temp);
        return pkt + (total_buffer_size - pkt_size - PKT_BUFFER_RIGHT_ROOM);
    }
    
    memcpy(pkt + (total_buffer_size - pkt_size - PKT_BUFFER_RIGHT_ROOM), 
        pkt, pkt_size);
    memset(pkt, 0, pkt_size);
    return pkt + (total_buffer_size - pkt_size - PKT_BUFFER_RIGHT_ROOM);
}

void
dump_interface_stats(interface_t *interface){

    printf("%s   ::  PktTx : %u, PktRx : %u, Pkt Egress Dropped : %u,  send rate = %lu bps",
        interface->if_name, interface->intf_nw_props.pkt_sent,
        interface->intf_nw_props.pkt_recv,
		interface->intf_nw_props.xmit_pkt_dropped,
        interface->intf_nw_props.bit_rate.bit_rate);
}

void
dump_node_interface_stats(node_t *node){

    interface_t *interface;

    uint32_t i = 0;

    for(; i < MAX_INTF_PER_NODE; i++){
        interface = node->intf[i];
        if(!interface)
            return;
        dump_interface_stats(interface);
        printf("\n");
    }
}

bool
is_interface_l3_bidirectional(interface_t *interface){

    /*if interface is in L2 mode*/
    if (IF_L2_MODE(interface) == ACCESS || 
        IF_L2_MODE(interface) == TRUNK)
        return false;

    /* If interface is not configured 
     * with IP address*/
    if (!IS_INTF_L3_MODE(interface))
        return false;

    interface_t *other_interface = &interface->link->intf1 == interface ?    \
            &interface->link->intf2 : &interface->link->intf1;

    if (!other_interface)
        return false;

    if (!IF_IS_UP(interface) ||
            !IF_IS_UP(other_interface)){
        return false;
    }

    if (IF_L2_MODE(other_interface) == ACCESS ||
        IF_L2_MODE(other_interface) == TRUNK)
        return false;

    if (!IS_INTF_L3_MODE(other_interface))
        return false;

    if (!(is_same_subnet(IF_IP(interface), IF_MASK(interface), 
        IF_IP(other_interface)) &&
        is_same_subnet(IF_IP(other_interface), IF_MASK(other_interface),
        IF_IP(interface)))){
        return false;
    }

    return true;
}

static void
interface_bit_rate_sample_update(event_dispatcher_t*ev_dis,
                                                        void *arg, uint32_t arg_size) {

    (unused)ev_dis;
    (unused)arg_size;

    if (!arg) return;
    
    interface_t *interface = (interface_t *)arg;

    interface->intf_nw_props.bit_rate.bit_rate = 
         interface->intf_nw_props.bit_rate.new_bit_stats - 
         interface->intf_nw_props.bit_rate.old_bit_stats;

    interface->intf_nw_props.bit_rate.old_bit_stats = 
         interface->intf_nw_props.bit_rate.new_bit_stats;
}

void
intf_init_bit_rate_sampling_timer(interface_t *interface) {

    wheel_timer_elem_t *wt_elem =
        interface->intf_nw_props.bit_rate.bit_rate_sampling_timer;

    assert(!wt_elem);

    wheel_timer_t *timer = DP_TIMER(interface->att_node);
    assert(timer);

    interface->intf_nw_props.bit_rate.bit_rate_sampling_timer =
        timer_register_app_event(timer, 
                                                 interface_bit_rate_sample_update,
                                                (void *)interface,
                                                sizeof(*interface),
                                                1000,
                                                1);
}

void
interface_loopback_create (node_t *node, uint8_t lono) {

    (unused) node;
    (unused) lono;
}

void
interface_loopback_delete (node_t *node, uint8_t lono) {

    (unused) node;
    (unused) lono;
}
