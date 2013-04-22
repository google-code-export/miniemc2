#ifndef _LINUX_FIQ_IPC_H
#define _LINUX_FIQ_IPC_H

/*
 * this defines the struct which is used to communicate between the FIQ
 * world and the normal linux kernel world.  One of these structs is
 * statically defined for you in the monolithic kernel so the FIQ ISR code
 * can safely touch it any any time.
 *
 * You also want to include this file in your kernel module that wants to
 * communicate with your FIQ code.  Add any kinds of vars that are used by
 * the FIQ ISR and the module in here.
 *
 * To get you started there is just an int that is incremented every FIQ
 * you can remove this when you are ready to customize, but it is useful
 * for testing
 */

#define TEST_PIN        204

#define AXIS_SET_IOCTL         1
#define PUSH_BUFF_IOCTL        3
#define PIN_CONF_IOCTL         7
#define PIN_FREE_IOCTL        15
#define PIN_TRANSFER_IOCTL     9
#define SCAN_PIN_SETUP_IOCTL  11
#define RB_SIZE_IOCTL	      13




#define MAX_AXIS 6
#define MAX_GPIO_PORTS  10
#define RINGBUFF_SIZE 128
#define MAX_PWM         2

#define PIN_IN      0
#define PIN_OUT     1

#define MAX_TRANS_PINS   20


typedef struct
{
  int axis_index;
  int step_pin;
  int step_pol;
  int dir_pin;
  int dir_pol;
  int slave_axis;
} axis_priv_config;


typedef struct
{
    int adder;
    int direction;
    long long cmd_position;
    int scan_sync;          // Set to 1 when X axis steps must be doubbled to a Scanner Sync pin
} step_domen_item;


typedef struct
{
    int num_reads; // number items dettached form tranfer_buffer in the last tranfer ops
    int underrun;   // Last stepgen underrun status
    int buff_free;  // Size of free space in the ringbuffer
    long long actual_pos[MAX_AXIS]; //Current axis coordinates
} stepgen_status;

typedef struct
{
    step_domen_item buffer[RINGBUFF_SIZE][MAX_AXIS];
    int buffsize;
    int PutPtr;
    int ringbuff_update;
} motion_data;

typedef struct axis_step_control
{
  unsigned int configured;
  unsigned int step_pin_addr;
  unsigned int step_pin_mask;
  unsigned int dir_pin_addr;
  unsigned int dir_pin_mask;
  int  dir_pin_pol;                 // Just invert direction pin polarity
  unsigned int phase_acc;                // Phase accumulator, use only 31 bit of 32
  unsigned int adder;                    // Adder, defined step pulses frequency
} axis_step_t;


struct gpio_ports
{
    int gpio_set_reg[MAX_GPIO_PORTS];
    int gpio_clr_reg[MAX_GPIO_PORTS];
};


struct fiq_ipc_static
{
    int rb_size;	//Ringbuffer size
    int cycle_per_ms; // Number of timer tisk per one msek
    int cycle_counter;    //Curent cycle counter
    int GetPtr;               //Ringbuff get pointer
    axis_step_t axis[MAX_AXIS];
//    int steps_act[MAX_AXIS];
    int scan_pin_num;
    int scan_pin_addr;
    int scan_pin_mask;       // Mask of a Scanner Sync pin control register
    int pwm_pin_addr[MAX_PWM];
    int pwm_pin_mask[MAX_PWM];
};


struct fiq_ipc_shared
{
    int underrun;
    long long step_count[MAX_AXIS];
    long long pos_error[MAX_AXIS];
    int gpios_changed;
    struct gpio_ports gpios;
    motion_data mdata;
    int pwm_duty_cycle[MAX_PWM];
};

/* actual definition lives in arch/arm/mach-s3c2440/fiq_c_isr.c */
extern struct fiq_ipc_shared *pfiq_ipc_shared;
extern struct fiq_ipc_static fiq_ipc_static;

extern unsigned long _fiq_count_fiqs;

extern void fiq_kick(void);  /* provoke a FIQ "immediately" */

#endif /* _LINUX_FIQ_IPC_H */

