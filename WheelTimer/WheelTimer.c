#include "WheelTimer.h"
#include <stdlib.h>
#include <stdio.h>
#include <memory.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>

#define TH_JOINABLE	1
#define TH_DETACHED	0

int
insert_wt_elem_in_slot(void *data1, void *data2){

   wheel_timer_elem_t *wt_elem1 = (wheel_timer_elem_t *)data1;
   wheel_timer_elem_t *wt_elem2 = (wheel_timer_elem_t *)data2;

   if(wt_elem1->execute_cycle_no < wt_elem2->execute_cycle_no)
       return -1;

   if(wt_elem1->execute_cycle_no > wt_elem2->execute_cycle_no)
       return 1;

   return 0;
}



/* Note : This wheel timer implementation do not maintain the list of events in
 * a slot in sorted manner based on r value of events. This is 
 * design defect. But as discussed in the tutorial, you need to
 * maintain the list of event sorted based on r value*/

wheel_timer_t*
init_wheel_timer(int wheel_size, int clock_tic_interval){
	wheel_timer_t *wt = calloc(1, sizeof(wheel_timer_t) + 
				(wheel_size * sizeof(slotlist_t)));

	wt->clock_tic_interval = clock_tic_interval;
	wt->wheel_size = wheel_size;

    memset(&(wt->wheel_thread), 0, sizeof(wheel_timer_t));

	int i = 0;
	for(; i < wheel_size; i++){
        init_glthread(WT_SLOTLIST_HEAD(wt, i));
        pthread_mutex_init(WT_SLOTLIST_MUTEX(wt, i), NULL);
    }

	return wt;
}

void
de_register_app_event(wheel_timer_elem_t *wt_elem){
    
    /* Next time when it shall be traversed by WT,
     * it shall be deleted*/
    wt_elem->valid = 0;
}

static void
process_wt_reschedule_slotlist(wheel_timer_t *wt){

    WT_LOCK_SLOT_LIST(WT_GET_RESCHD_SLOTLIST(wt));
    if(WT_IS_SLOTLIST_EMPTY(WT_GET_RESCHD_SLOTLIST(wt))){
        WT_UNLOCK_SLOT_LIST(WT_GET_RESCHD_SLOTLIST(wt));
        return;
    }

    glthread_t *curr;
    wheel_timer_elem_t *wt_elem;

    ITERATE_GLTHREAD_BEGIN(WT_GET_RESCHD_SLOTLIST_HEAD(wt), curr){

        wt_elem = glthread_reschedule_glue_to_wt_elem(curr);
        WT_LOCK_WTELEM_SLOT_LIST(wt_elem);
        remove_glthread(&wt_elem->glue);
        WT_UNLOCK_WTELEM_SLOT_LIST(wt_elem);
        wt_elem->slotlist_head = NULL;

        /*relocate Or reschedule to the next slot*/ 
        int absolute_slot_no = GET_WT_CURRENT_ABS_SLOT_NO(wt);
        int next_abs_slot_no  = absolute_slot_no + 
            (wt_elem->time_interval/wt->clock_tic_interval);
        int next_cycle_no     = next_abs_slot_no / wt->wheel_size;
        int next_slot_no      = next_abs_slot_no % wt->wheel_size;
        wt_elem->execute_cycle_no    = next_cycle_no;
        wt_elem->slot_no = next_slot_no;

        /*lock the list to which we are shifting the wt_elem, because the 
         * application can also invoke register_app_event on same slotlist*/
        WT_LOCK_SLOT_LIST(WT_SLOTLIST(wt, next_slot_no));
        glthread_priority_insert(WT_SLOTLIST_HEAD(wt, next_slot_no), &wt_elem->glue,
                insert_wt_elem_in_slot, 
                (unsigned long)&((wheel_timer_elem_t *)0)->glue);
        WT_UNLOCK_SLOT_LIST(WT_SLOTLIST(wt, next_slot_no));

        wt_elem->slotlist_head = WT_SLOTLIST(wt, next_slot_no);
        remove_glthread(&wt_elem->reschedule_glue);

    }ITERATE_GLTHREAD_END(WT_GET_RESCHD_SLOTLIST_HEAD(wt), curr)
    WT_UNLOCK_SLOT_LIST(WT_GET_RESCHD_SLOTLIST(wt));
}


static void *
wheel_fn(void *arg){

	wheel_timer_t *wt = (wheel_timer_t *)arg;
	wheel_timer_elem_t *wt_elem = NULL;
	int absolute_slot_no = 0, i =0;
    slotlist_t *slot_list = NULL;
	glthread_t *curr;

	while(1){
        
        wt->current_clock_tic++;
        if(wt->current_clock_tic == wt->wheel_size)
            wt->current_clock_tic = 0;

		if(wt->current_clock_tic == 0)
			wt->current_cycle_no++;

		sleep(wt->clock_tic_interval);

		slot_list = WT_SLOTLIST(wt, wt->current_clock_tic);
		absolute_slot_no = GET_WT_CURRENT_ABS_SLOT_NO(wt);

         WT_LOCK_SLOT_LIST(slot_list);
		 ITERATE_GLTHREAD_BEGIN(&slot_list->slots, curr){

            wt_elem = glthread_to_wt_elem(curr);
           
            /*application had first rescheduled the wt_elem, then later decided
             * to de-register it*/
            if(wt_elem->valid == 0){
                remove_glthread(curr);
                if(!IS_GLTHREAD_LIST_EMPTY(&wt_elem->reschedule_glue)){
                    WT_LOCK_SLOT_LIST(WT_GET_RESCHD_SLOTLIST(wt));
                    remove_glthread(&wt_elem->reschedule_glue);
                    WT_UNLOCK_SLOT_LIST(WT_GET_RESCHD_SLOTLIST(wt));
                }
                free_wheel_timer_element(wt_elem);
                continue;
            }
            
            /*If it is rescheduled again, dont process it*/
            if(!IS_GLTHREAD_LIST_EMPTY(&wt_elem->reschedule_glue)){
                continue;
            }
            
            /*Check if R == r*/
			if(wt->current_cycle_no == wt_elem->execute_cycle_no){
                /*Invoke the application event through fn pointer as below*/
				wt_elem->app_callback(wt_elem->arg, wt_elem->arg_size);

                /* After invocation, check if the event needs to be rescheduled again
                 * in future*/
				if(wt_elem->is_recurrence){

                    /* Could be possible that appl cb itself has de-register the WT
                     * element*/
                    if(wt_elem->valid == 0){
                        remove_glthread(curr);
                        if(!IS_GLTHREAD_LIST_EMPTY(&wt_elem->reschedule_glue)){
                            WT_LOCK_SLOT_LIST(WT_GET_RESCHD_SLOTLIST(wt));
                            remove_glthread(&wt_elem->reschedule_glue);
                            WT_UNLOCK_SLOT_LIST(WT_GET_RESCHD_SLOTLIST(wt));
                        }
                        free_wheel_timer_element(wt_elem);
                        continue;
                    }
                    /*Or else the applicatoin has rescheduled the event again,
                     * then remove the event from reschedule thread we WT is 
                     * anyway going to reschedule it now*/
                    else if(!IS_GLTHREAD_LIST_EMPTY(&wt_elem->reschedule_glue)){
                        WT_LOCK_SLOT_LIST(WT_GET_RESCHD_SLOTLIST(wt));
                        remove_glthread(&wt_elem->reschedule_glue);
                        WT_UNLOCK_SLOT_LIST(WT_GET_RESCHD_SLOTLIST(wt));
                    }
					
                    /*relocate Or reschedule to the next slot*/
					int next_abs_slot_no  = absolute_slot_no + 
                            (wt_elem->time_interval/wt->clock_tic_interval);
					int next_cycle_no     = next_abs_slot_no / wt->wheel_size;
					int next_slot_no      = next_abs_slot_no % wt->wheel_size;
					wt_elem->execute_cycle_no 	 = next_cycle_no;

                    /* It might be possible that next slot no could be same as 
                     * where the current clock time is. Inb this case, simply
                     * update the r value and adjust the wt_elem position
                     * in the linked list in the increasing order of r value*/
					if(next_slot_no == wt->current_clock_tic){
                        remove_glthread(curr);
                        glthread_priority_insert(WT_SLOTLIST_HEAD(wt, next_slot_no), &wt_elem->glue,
                            insert_wt_elem_in_slot,
                            (unsigned long)&((wheel_timer_elem_t *)0)->glue);
						continue;
					}
                    /*Remove from Event from the old slot*/
                    remove_glthread(curr);

                    /*Add the event to the new slot, This slotlist cannot be same
                     * as current slotlist being processed by WT*/
                    WT_LOCK_SLOT_LIST(WT_SLOTLIST(wt, next_slot_no));
					glthread_priority_insert(WT_SLOTLIST_HEAD(wt, next_slot_no), &wt_elem->glue, 
                                    insert_wt_elem_in_slot, 
                                    (unsigned long)&((wheel_timer_elem_t *)0)->glue);
                    WT_UNLOCK_SLOT_LIST(WT_SLOTLIST(wt, next_slot_no));
                    wt_elem->slotlist_head = WT_SLOTLIST(wt, next_slot_no);
				}
				else{
                    /*If application has rescheduled the event again*/
                    if(!IS_GLTHREAD_LIST_EMPTY(&wt_elem->reschedule_glue)){
                        /* No Action required, during rescheduling the wt_elem
                         * will be removed from current slot*/
                    }
                    else if(wt_elem->valid){
                        assert(0);/*Application is suppose to de-register the event*/
                    }
                    else if(wt_elem->valid == 0){
                        remove_glthread(curr);
                        free_wheel_timer_element(wt_elem);
                    }
				}
			}
        } ITERATE_GLTHREAD_END(slot_list, curr)
        WT_UNLOCK_SLOT_LIST(slot_list);
        process_wt_reschedule_slotlist(wt);
	}
	return NULL;
}

wheel_timer_elem_t *
register_app_event(wheel_timer_t *wt,
		app_call_back call_back,
		void *arg,
		int arg_size,
		int time_interval,
		char is_recursive){

	if(!wt || !call_back) return NULL;
	wheel_timer_elem_t *wt_elem = calloc(1, sizeof(wheel_timer_elem_t));

	wt_elem->time_interval = time_interval;
	wt_elem->app_callback  = call_back;
	wt_elem->arg 	       = calloc(1, arg_size);
	memcpy(wt_elem->arg, arg, arg_size);
	wt_elem->arg_size      = arg_size;
	wt_elem->is_recurrence = is_recursive;
    init_glthread(&wt_elem->glue);
	int wt_absolute_slot = GET_WT_CURRENT_ABS_SLOT_NO(wt);
	int registration_next_abs_slot = wt_absolute_slot + (wt_elem->time_interval/wt->clock_tic_interval);
	int cycle_no = registration_next_abs_slot / wt->wheel_size;
	int slot_no  = registration_next_abs_slot % wt->wheel_size;
	wt_elem->execute_cycle_no = cycle_no;
    wt_elem->slot_no = slot_no;
    WT_LOCK_SLOT_LIST(WT_SLOTLIST(wt, slot_no));
    glthread_priority_insert(WT_SLOTLIST_HEAD(wt, slot_no), &wt_elem->glue, 
            insert_wt_elem_in_slot, 
            (unsigned long)&((wheel_timer_elem_t *)0)->glue);
    WT_UNLOCK_SLOT_LIST(WT_SLOTLIST(wt, slot_no));
    wt_elem->slotlist_head = WT_SLOTLIST(wt, slot_no);
    wt_elem->valid = 1;
	return wt_elem;
}

int
wt_get_remaining_time(wheel_timer_t *wt, 
                    wheel_timer_elem_t *wt_elem){

    int wt_absolute_slot = GET_WT_CURRENT_ABS_SLOT_NO(wt);
    int wt_elem_absolute_slot = (wt_elem->execute_cycle_no * wt->wheel_size) + 
            wt_elem->slot_no;
    int diff = wt_elem_absolute_slot - wt_absolute_slot;
    assert(diff >= 0);
    return (diff * wt->clock_tic_interval);
}

void
free_wheel_timer_element(wheel_timer_elem_t *wt_elem){
    
    wt_elem->slotlist_head = NULL;
	free(wt_elem->arg);
	free(wt_elem);
}


void
print_wheel_timer(wheel_timer_t *wt){
	
    int i = 0, j = 0;
	glthread_t *curr;
    glthread_t *slot_list_head = NULL;
	wheel_timer_elem_t *wt_elem = NULL;

	printf("Printing Wheel Timer DS\n");
	printf("wt->current_clock_tic  = %d\n", wt->current_clock_tic);
	printf("wt->clock_tic_interval = %d\n", wt->clock_tic_interval);
	printf("wt->wheel_size         = %d\n", wt->wheel_size);
	printf("wt->current_cycle_no   = %d\n", wt->current_cycle_no);
	printf("wt->wheel_thread       = %p\n", &wt->wheel_thread);
	printf("printing slots : \n");

	for(; i < wt->wheel_size; i++){
        slot_list_head = WT_SLOTLIST_HEAD(wt, i);
        ITERATE_GLTHREAD_BEGIN(slot_list_head, curr){
            wt_elem = glthread_to_wt_elem(curr); 
			printf("                wt_elem->time_interval		= %d\n",  wt_elem->time_interval);
			printf("                wt_elem->execute_cycle_no	= %d\n",  wt_elem->execute_cycle_no);
			printf("                wt_elem->app_callback		= %p\n",  wt_elem->app_callback);
			printf("                wt_elem->arg    			= %p\n",  wt_elem->arg);
			printf("                wt_elem->is_recurrence		= %d\n",  wt_elem->is_recurrence);
            printf("                wt_elem->valid              = %d\n",  wt_elem->valid);
            printf("                is sched ?                  = %s\n",  (!IS_GLTHREAD_LIST_EMPTY(&wt_elem->reschedule_glue) ? "Y":"N"));
        } ITERATE_GLTHREAD_END(slot_list_head , curr)
	}
}


void
start_wheel_timer(wheel_timer_t *wt){

	if (pthread_create(&wt->wheel_thread, NULL, wheel_fn, (void*)wt))
	{
		printf("Wheel Timer Thread initialization failed, exiting ... \n");
		exit(0);
	}
}

void
reset_wheel_timer(wheel_timer_t *wt){
	wt->current_clock_tic = 0;
	wt->current_cycle_no  = 0;
}

void
wt_elem_reschedule(wheel_timer_t *wt, 
                   wheel_timer_elem_t *wt_elem, 
                   int new_time_interval){
   
    assert(wt_elem->valid);
    
    wt_elem->time_interval = new_time_interval;
    
    if(!IS_GLTHREAD_LIST_EMPTY(&wt_elem->reschedule_glue))
        return;

    WT_LOCK_SLOT_LIST(WT_GET_RESCHD_SLOTLIST(wt));
    glthread_add_next(WT_GET_RESCHD_SLOTLIST_HEAD(wt), 
        &wt_elem->reschedule_glue);
    WT_UNLOCK_SLOT_LIST(WT_GET_RESCHD_SLOTLIST(wt));
}

