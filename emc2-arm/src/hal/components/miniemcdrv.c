#ifndef RTAPI
#error This is a realtime component only!
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <linux/types.h>
#include <sys/mman.h>
#include <sys/times.h>
#include <sys/mman.h>
#include <limits.h>
#include "rtapi.h"              /* RTAPI realtime OS API */
#include "rtapi_app.h"          /* RTAPI realtime module decls */
#include "rtapi_errno.h"        /* EINVAL etc */
#include "hal.h"                /* HAL public API decls */


#include <arch/arm/mach-s3c2410/include/mach/fiq_ipc_mini2440.h>


#define SCAN_SYNC_PIN 197



/* module information */
MODULE_AUTHOR("Sergey Kaydalov");
MODULE_DESCRIPTION("miniEMC stepgen agent");
MODULE_LICENSE("GPL");
static int num_axis = 0;        /* number of channels - default = 3 */
static int fifo_deep = 1;      /* FIQ stepgen fifo size, up to 128. Less size - less fifo delay time, but also less stability on the non-realtime system*/
RTAPI_MP_INT(fifo_deep, "deepest of spi fifo");

static char* axes_conf = "";
RTAPI_MP_STRING(axes_conf, "Axes configuration string");

static int scaner_compat = 0;      /* Support for Scaner enable/disable*/
RTAPI_MP_INT(scaner_compat, "Enable 3D Scaner compatibility");

static int io_update_period = 1;      /* Number of tics per single IO pin update*/
RTAPI_MP_INT(io_update_period, "io update period");

int step_per_unit[MAX_AXIS] = { 320000, 320000, 320000, 3200*100, 3200*100, 3200*100 };
RTAPI_MP_ARRAY_INT(step_per_unit, MAX_AXIS,"Number of steps per unit multiplied by 100, for up to 6 channels");


int step_pins[MAX_AXIS] = { -1, -1, -1, -1, -1, -1 };
RTAPI_MP_ARRAY_INT(step_pins,MAX_AXIS,"stepping pin numbers for up to 6 channels");

int dir_pins[MAX_AXIS] = { -1, -1, -1, -1, -1, -1 };
RTAPI_MP_ARRAY_INT(dir_pins,MAX_AXIS,"direction pin numbers for up to 6 channels");

int dir_polarity[MAX_AXIS] = { 0, 0, 0, 0, 0, 0 };
RTAPI_MP_ARRAY_INT(dir_polarity,MAX_AXIS,"polarity of direction pins (0 or 1) for up to 6 channels");

int pwm_pin_num[MAX_PWM] = { -1, -1 };
RTAPI_MP_ARRAY_INT(pwm_pin_num, 2, "PWM pin index");

int max_pwm_value = 10000;
RTAPI_MP_INT(max_pwm_value, "PWM frequency scaling factor, max value ");


/*
*  Slave axis - such axis, that not have its own commanded position signal, but used position from master axis
*  For each master axis we can setup one slave axis specifying his index in the slave_axis array
*/

int axis_map[MAX_AXIS] = { -1, -1, -1 , -1 , -1, -1 };


/***********************************************************************
*                STRUCTURES AND GLOBAL VARIABLES                       *
************************************************************************/

/* this structure contains the runtime data for a single counter */




typedef struct
{
    hal_float_t *cmd_pos[MAX_AXIS]; // input pins, commanded axis position
    hal_float_t *fb_pos[MAX_AXIS];  // output pins, feedback axis position
    hal_float_t *pwm_duty[2];       //PWM duty cycle
    hal_bit_t	*io_pin[100];         // output pin to trajectory planer sync pin
    hal_bit_t	*io_invert[100];         // output pin to trajectory planer sync pin
    hal_bit_t	*traj_wait;         // output pin to trajectory planer sync pin
    hal_bit_t	*scan_sync;
    int fd;
    struct fiq_ipc_shared* pfiq;
 } gpio_t;

/* pointer to array of counter_t structs in shmem, 1 per counter */
static gpio_t *pgpio;

static long long cmd_pos_prev[MAX_AXIS];
static long long cmd_pos_accum[MAX_AXIS];
/* other globals */
static int comp_id;		/* component ID */

/***********************************************************************
*                  GPIO ACCESS FUNCTIONS                               *
************************************************************************/
typedef enum { egpNone = 0, egpIn, egpOut, egpPerif, egpRsv } egpMode;
typedef struct
{
    int port_index;
	int PCON;
	int PDAT;
	int 	   offset;
	egpMode    mode;
	const char name[16];
} miniemcGPIO;


#ifdef CONFIG_MARCH_MINI2416

#define GPIO_MAP_FADDR 0x56000010
#define GPIO_MAP_SIZE	0xC0


const miniemcGPIO GPIOS[] =
{
    //Outputs
   { 1, 0x56000020, 0x56000024,  4, egpOut, "GPC04" }	// 0
 , { 1, 0x56000020, 0x56000024, 10, egpOut, "GPC10" }	// 1
 , { 1, 0x56000020, 0x56000024, 11, egpOut, "GPC11" }	// 2
 , { 1, 0x56000020, 0x56000024, 12, egpOut, "GPC12" }	// 3
 , { 0, 0x56000010, 0x56000014,  3, egpOut, "GPB03" }	// 4
 , { 1, 0x56000020, 0x56000024,  2, egpOut, "GPC02" }	// 5
 , { 1, 0x56000020, 0x56000024,  1, egpOut, "GPC01" }	// 6
 , { 1, 0x56000020, 0x56000020, 3,  egpOut, "GPC03" }	// 7
 , { 2, 0x56000030, 0x56000034, 3,  egpOut, "GPD03" }	// 8
 , { 2, 0x56000030, 0x56000034, 4,  egpOut, "GPD04" }	// 9
 , { 2, 0x56000030, 0x56000034, 5,  egpOut, "GPD05" }	// 10
 , { 2, 0x56000030, 0x56000034, 6,  egpOut, "GPD06" }	// 11
 , { 1, 0x56000020, 0x56000024, 13, egpOut,  "GPC13" }	// 12
 , { 1, 0x56000020, 0x56000024, 14, egpOut,  "GPC14" }	// 13
 , { 1, 0x56000020, 0x56000024, 15, egpOut,  "GPC15" }	// 14
 , { 2, 0x56000030, 0x56000034,  2, egpOut,  "GPD02" }	// 15
 , { 2, 0x56000030, 0x56000034,  7, egpOut, "GPD07" }	// 16
 , { 2, 0x56000030, 0x56000034, 10, egpOut, "GPD10" }	// 17
 , { 2, 0x56000030, 0x56000034, 11, egpOut, "GPD11" }	// 18
 , { 2, 0x56000030, 0x56000034, 12, egpOut, "GPD12" }	// 19
 , { 2, 0x56000030, 0x56000034, 13, egpOut, "GPD13" }	// 20
 , { 2, 0x56000030, 0x56000034, 14, egpOut, "GPD14" }	// 21
 , { 2, 0x56000030, 0x56000034, 15, egpOut, "GPD15" }	// 22
    //Inputs
 , { 3, 0x56000040, 0x56000044, 10, egpIn, "GPE10" }	// 0
 , { 3, 0x56000040, 0x56000044, 9, egpIn, "GPE09" }	// 1
 , { 3, 0x56000040, 0x56000044, 8, egpIn, "GPE08" }	// 2
 , { 3, 0x56000040, 0x56000044, 7, egpIn, "GPE07" }	// 3
 , { 3, 0x56000040, 0x56000044, 6, egpIn, "GPE06" }	// 4
 , { 3, 0x56000040, 0x56000044, 5, egpIn, "GPE05" }	// 5
 , { 4, 0x56000050, 0x56000044, 0, egpIn, "GPF00" }	// 6
 , { 4, 0x56000050, 0x56000054, 3, egpIn,"GPF03" }	// 7
 , { 4, 0x56000050, 0x56000054, 5, egpIn,"GPF05" }	// 8
 , { 4, 0x56000050, 0x56000054, 6, egpIn,"GPF06" }	// 9
 , { 4, 0x56000050, 0x56000054, 7, egpIn,"GPF07" }	// 10
 , { 4, 0x56000050, 0x56000054, 8, egpIn,"GPF08" }	// 11
 , { 4, 0x56000050, 0x56000054, 9, egpIn,"GPF09" }	// 12
 , { 4, 0x56000050, 0x56000054, 10, egpIn,"GPF10" }	// 13
 , { 4, 0x56000050, 0x56000054, 11, egpIn,"GPF11" }	// 14
 , { 4, 0x56000050, 0x56000054, 1, egpIn,"GPF01" }	// 15
};
#else

#define GPIO_MAP_FADDR 0x56000040
#define GPIO_MAP_SIZE	0x90


const miniemcGPIO GPIOS[] =
{
   // Output pins
   { 2, 0x56000050, 0x56000054,  0, egpOut, "GPF00" }	// 0
 , { 2, 0x56000050, 0x56000054,  1, egpOut, "GPF01" }	// 1
 , { 2, 0x56000050, 0x56000054,  2, egpOut, "GPF02" }	// 2
 , { 2, 0x56000050, 0x56000054,  3, egpOut, "GPF03" }	// 3
 , { 2, 0x56000050, 0x56000054,  4, egpOut, "GPF04" }	// 4
 , { 2, 0x56000050, 0x56000054,  5, egpOut, "GPF05" }	// 5
 , { 2, 0x56000050, 0x56000054,  6, egpOut, "GPF06" }	// 6
 , { 3, 0x56000060, 0x56000064, 0,  egpOut, "GPG00" }	// 7
 , { 3, 0x56000060, 0x56000064, 1,  egpOut, "GPG01" }	// 8
 , { 3, 0x56000060, 0x56000064, 3,  egpOut, "GPG03" }	// 9
 , { 3, 0x56000060, 0x56000064, 5,  egpOut, "GPG05" }	// 10
 , { 3, 0x56000060, 0x56000064, 6,  egpOut, "GPG06" }	// 11
 , { 4, 0x560000d0, 0x560000d4, 5,  egpOut, "GPJ05" }	// 12
 , { 4, 0x560000d0, 0x560000d4, 4,  egpOut, "GPJ04" }	// 13
 , { 4, 0x560000d0, 0x560000d4, 3,  egpOut, "GPJ03" }	// 14
 , { 4, 0x560000d0, 0x560000d4, 2,  egpOut, "GPJ02" }	// 15
 , { 4, 0x560000d0, 0x560000d4, 1,  egpOut, "GPJ01" }	// 16
 , { 4, 0x560000d0, 0x560000d4, 0,  egpOut,"GPJ00" }	// 17
 , { 0, 0x56000010, 0x56000014, 0,  egpOut,"GPB00" }	// 18
// Input pins
 , { 3, 0x56000060, 0x56000064, 7,  egpIn,  "GPG07" }	// 0
 , { 3, 0x56000060, 0x56000064, 9,  egpIn,  "GPG09" }	// 1
 , { 3, 0x56000060, 0x56000064, 10, egpIn,  "GPG10" }	// 2
 , { 3, 0x56000060, 0x56000064, 11, egpIn,  "GPG11" }	// 3
 , { 1, 0x56000040, 0x56000044, 11, egpIn, "GPE11" }	// 4
 , { 1, 0x56000040, 0x56000044, 12, egpIn, "GPE12" }	// 5
 , { 1, 0x56000040, 0x56000044, 13, egpIn, "GPE13" }	// 6
 , { 3, 0x56000060, 0x56000064, 10, egpIn, "GPG02" }	// 7
 , { 1, 0x56000040, 0x56000044, 15, egpIn, "GPE15" }	// 8
 , { 1, 0x56000040, 0x56000044, 14, egpIn, "GPE14" }	// 9
 , { 3, 0x56000060, 0x56000064, 12, egpIn, "GPG12" }	// 10
 , { 4, 0x560000d0, 0x560000d4, 12, egpIn, "GPJ12" }	// 11
 , { 4, 0x560000d0, 0x560000d4, 11, egpIn, "GPJ11" }	// 12
 , { 4, 0x560000d0, 0x560000d4, 10, egpIn, "GPJ10" }	// 13
 , { 4, 0x560000d0, 0x560000d4, 9,  egpIn, "GPJ09" }	// 14
 , { 4, 0x560000d0, 0x560000d4, 8,  egpIn, "GPJ08" }	// 15
 , { 4, 0x560000d0, 0x560000d4, 7,  egpIn, "GPJ07" }	// 16
 , { 4, 0x560000d0, 0x560000d4, 6,  egpIn, "GPJ06" }	// 17
 , { 0, 0x56000010, 0x56000014, 1,  egpIn,"GPB01" }	// 18
};
#endif


#define ARR_SZ(x) ( sizeof(x)/sizeof(x[0]) )
static int* iomem = NULL;
static char gpio_in_use[  ARR_SZ(GPIOS) ] = {0};


/***********************************************************************
*                  IO MEMORY MAPPERS                                   *
************************************************************************/

#define MAP_SIZE			4096UL
#define MAP_MASK 			(MAP_SIZE - 1)

void *mapIoRegister(unsigned long addr, size_t length)
{
	void *map_base, * virtAddr;
	off_t target = ((unsigned int)addr) & ~MAP_MASK;
	int fd;

	if ((fd = open("/dev/mem", O_RDWR | O_SYNC)) == -1) {
		printf("/dev/mem could not be opened.\n");
		return NULL;
	}

	/* Map one page */
	map_base = mmap((void *)target, length, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, target);
	if (map_base == (void *) -1) {
		printf("Memory map failed for address 0x%lx\n", addr);
		return NULL;
	}

	virtAddr = (void *)((unsigned long)map_base +
			    ((unsigned long)addr & MAP_MASK));
	printf("Memory map 0x%lx -> %p offset 0x%lx virtual %p\n",
		addr, map_base, addr & MAP_MASK, virtAddr);
	return virtAddr;
}

int iounmap(volatile void *start, size_t length)
{
	unsigned long ofs_addr;
	ofs_addr = (unsigned long)start & (getpagesize()-1);

	/* do some cleanup when you're done with it */
	return munmap((void*)start-ofs_addr, length+ofs_addr);
}

/***********************************************************************
*                  GPIO manager                                        *
************************************************************************/
int emcConfigurePin( int index , egpMode mode )
{
	if( index < 0 || index >= ARR_SZ(GPIOS) )
		return -1;
	if( gpio_in_use[index] != egpNone )
		return -2;
	if( iomem == NULL )
	{
		iomem = mapIoRegister( GPIO_MAP_FADDR, GPIO_MAP_SIZE );
		if( iomem == NULL)
			return -3;
	}
	int pcon_of = (GPIOS[index].PCON - GPIO_MAP_FADDR)/sizeof(int);
	int of_idx     = GPIOS[index].offset;

	// Clear GPIO mode to zero (input )
	*(iomem + pcon_of ) &= ~(3L << (of_idx*2));
	if( mode == egpOut )
	{
		*(iomem + pcon_of) |= (1L << (of_idx*2));
	} else if( mode == egpPerif )
	{
	   *(iomem + pcon_of) |= (2L << (of_idx*2));
	}
	//printf( "PCON[%p]=%x\n",(void*)(iomem + pcon_of), *(iomem + pcon_of ) );
	gpio_in_use[index] = mode;
	return 0;
}

#define emcConfigureDefault(index) emcConfigurePin(index,  GPIOS[index].mode )

int emcDeConfigurePin( int index )
{
	if( index < 0 || index >= ARR_SZ(GPIOS) )
		return -1;
	if( gpio_in_use[index] == egpNone )
		return -2;
	if( iomem == NULL )
	{
		iomem = mapIoRegister( GPIO_MAP_FADDR, GPIO_MAP_SIZE );
		if( iomem == NULL)
			return -3;
	}
	int pcon_of = (GPIOS[index].PCON - GPIO_MAP_FADDR)/sizeof(int);
	int of_idx     = GPIOS[index].offset;

	// Clear GPIO mode to zero (input )
	*(iomem + pcon_of ) &= ~(3L << (of_idx*2));
	gpio_in_use[index] = egpNone;
	return 0;
}

int emcReservePin( int index )
{
	if( index < 0 || index >= ARR_SZ(GPIOS) )
		return -1;
	if( gpio_in_use[index] == egpNone )
		return -2;
	gpio_in_use[index] = egpRsv;
	return 0;
}

egpMode emcGetPinMode( int index )
{
	if( index < 0 || index >= ARR_SZ(GPIOS) )
		return -1;
	return GPIOS[index].mode;
}

int emcIsPinConfigured( int index )
{
	if( index < 0 || index >= ARR_SZ(GPIOS) )
		return -1;
	return gpio_in_use[index] != egpNone;
}

void emcSetPin(int index, int value )
{
	if( index < 0 || index >= ARR_SZ(GPIOS) )
		return;
	if(  gpio_in_use[index] == egpOut )
	{
		if(pgpio->pfiq )
		{
		    int of_idx  = 1 << GPIOS[index].offset;
		    if( value )
                pgpio->pfiq->gpios.gpio_set_reg[GPIOS[index].port_index] |= of_idx;
            else
                pgpio->pfiq->gpios.gpio_clr_reg[GPIOS[index].port_index] |= of_idx;
 		}
	}
}

int emcGetPin( int index )
{
	if( index < 0 || index >= ARR_SZ(GPIOS) )
		return -1;

	if(  gpio_in_use[index] != egpNone )
	{
		int pdat_of = (GPIOS[index].PDAT - GPIO_MAP_FADDR)/sizeof(int);
		int of_idx  = GPIOS[index].offset;
		return (*(iomem + pdat_of ) & (1LL << of_idx)) != 0;
	}
	return -2;
}

int emcGetPinByName( const char* PinName )
{
    int i, rc = -1;
    for( i =0; i <  ARR_SZ(GPIOS); i++ )
    {
        if( strcmp( PinName, GPIOS[i].name) == 0 )
        {
            rc = i;
            break;
        }
    }
    printf("%s index=%d\n", PinName, i );
    return i;
}

/***********************************************************************
*                  PWM TIMER      DECLARATIONS                         *
************************************************************************/

void emcPWMSetDutyCycle(int index, int value )
{
    if (index >=0 && index < MAX_PWM && pgpio->pfiq )
    {
        int val = abs(value)*100.0f/max_pwm_value;
        pgpio->pfiq->pwm_duty_cycle[index] = 99 - val;
//        printf("pwm[%d].value=%d\n", index, val);
    }

}

/***********************************************************************
*                  LOCAL FUNCTION DECLARATIONS                         *
************************************************************************/

static void update(void *arg, long period);

void rtapi_app_exit(void);

/***********************************************************************
*                       INIT AND EXIT CODE                             *
************************************************************************/
struct fiq_ipc_static fiq_static = {0};

int rtapi_app_main(void)
{
    int retval, i;
    int in_cnt = 0, out_cnt = 0;
    char buf[128];
    char *pc = axes_conf;

    /* Parsing axes configuration */
    while(*pc != 0)
    {
        int idx = -1;
        switch(*pc)
        {
            case 'X':
            case 'x':
                    idx = 0;
                    break;
            case 'Y':
            case 'y':
                    idx = 1;
                    break;

            case 'Z':
            case 'z':
                    idx = 2;
                    break;

            case 'A':
            case 'a':
                    idx = 3;
                    break;

            case 'B':
            case 'b':
                    idx = 4;
                    break;

            case 'C':
            case 'c':
                    idx = 5;
                    break;

            default: break;
        }
        if(idx >= 0)
            axis_map[num_axis++] = idx;
        pc++;
    }

    fprintf(stderr, "num_axis=%d, fifo_size=%d\n", num_axis, fifo_deep);

    /* test for number of channels */
    if ((num_axis <= 0) || (num_axis > MAX_AXIS)) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "miniemcdrv: ERROR: invalid num_chan: %d\n", num_axis);
	return -EINVAL;
    }
    /* have good config info, connect to the HAL */
    comp_id = hal_init("miniemcdrv");
    if (comp_id < 0)
    {
        rtapi_print_msg(RTAPI_MSG_ERR, "miniemcdrv: ERROR: hal_init() failed\n");
        rtapi_app_exit();
        return -EINVAL;
    }
    pgpio = hal_malloc(sizeof(gpio_t));
    memset(pgpio, 0, sizeof(gpio_t));

    pgpio->fd = open("/dev/miniemc", O_RDWR | O_SYNC);

    if(pgpio->fd < 0)
    {
        rtapi_print_msg(RTAPI_MSG_ERR,
            "miniemcdrv: ERROR: unble to create access to stepgen module\n");
        rtapi_app_exit();
        return -EIO;
    }

    pgpio->pfiq = mmap(0, sizeof(struct fiq_ipc_shared), PROT_READ | PROT_WRITE, MAP_FILE | MAP_SHARED, pgpio->fd, 0);
	if(pgpio->pfiq == MAP_FAILED)
	{
        rtapi_print_msg(RTAPI_MSG_ERR,
            "miniemcdrv: ERROR: unable to mmap stepgen ringbuffer\n");
        rtapi_app_exit();
        return -EIO;
	}

     /* Setup ringbuff size */
    fiq_static.rb_size = fifo_deep;

    memset(&cmd_pos_prev, 0, sizeof(cmd_pos_prev));
    memset(&cmd_pos_accum, 0, sizeof(cmd_pos_accum));

    //Configure PWM pins and create HAL inputs
    for( i = 0; i < MAX_PWM; i++)
    {
        rtapi_snprintf(buf, HAL_NAME_LEN, "miniemcdrv.%d.pwm-in", i);
        retval = hal_pin_float_new(buf, HAL_IN, &(pgpio->pwm_duty[i]), comp_id);
        if (retval != 0)
        {
            rtapi_app_exit();
            return retval;
        }

        if( pwm_pin_num[i] >= 0 )
        {
            emcConfigureDefault( pwm_pin_num[i] );
            emcReservePin( pwm_pin_num[i] );
           fiq_static.pwm_pin_addr[i] =  GPIOS[pwm_pin_num[i]].port_index;
           fiq_static.pwm_pin_mask[i] =  1L << GPIOS[pwm_pin_num[i]].offset;
           pgpio->pfiq->pwm_duty_cycle[i] = 0;
        } else
        {
            fiq_static.pwm_pin_mask[i] = 0;
            fiq_static.pwm_pin_addr[i] = 0;
        }
    }

    // Create axis step and dir pins
    for(i = 0; i < num_axis; i++)
    {
        if(step_pins[i] >=0 && dir_pins[i] >= 0)
        {
            if( emcGetPinMode( step_pins[i]) == egpIn ||  emcGetPinMode( dir_pins[i]) == egpIn )
            {
                rtapi_print_msg(RTAPI_MSG_ERR, "WARN: can't create axis[%d] stepgen, invalid pin\n", i);
                continue;
            }
            fiq_static.axis[i].configured = 0;
            fiq_static.axis[i].step_pin_addr = GPIOS[step_pins[i]].port_index;
            fiq_static.axis[i].step_pin_mask = 1L << GPIOS[step_pins[i]].offset;
            emcConfigureDefault( step_pins[i] );
            emcReservePin( step_pins[i] );
            fiq_static.axis[i].dir_pin_addr = GPIOS[dir_pins[i]].port_index;
            fiq_static.axis[i].dir_pin_mask = 1L << GPIOS[dir_pins[i]].offset;
            emcConfigureDefault( dir_pins[i] );
            emcReservePin( dir_pins[i] );
            fiq_static.axis[i].dir_pin_pol = dir_polarity[i];
            fiq_static.axis[i].configured = 1;
        } else
        {
            rtapi_print_msg(RTAPI_MSG_ERR,
                "miniemcdrv: WARNING: axis[%d] step and/or dir pin(s) not properly configured, skipping\n", i);
        }
        fiq_static.scan_pin_num = -1;
    }

    ioctl(pgpio->fd, AXIS_SET_IOCTL, &fiq_static );
    /*
     * Create IO pins
     */

    for( i=0; i < ARR_SZ(GPIOS); i++ )
    {
        if(  emcGetPinMode( i ) == egpRsv )
            continue;
        if( emcGetPinMode( i ) == egpIn )
        {
            rtapi_snprintf(buf, HAL_NAME_LEN, "miniemcdrv.%d.pin-in", in_cnt);
            hal_pin_bit_new(buf, HAL_OUT, &(pgpio->io_pin[i]), comp_id);
            rtapi_snprintf(buf, HAL_NAME_LEN, "miniemcdrv.%d.pin-in-inv", in_cnt);
            hal_pin_bit_new(buf, HAL_IN, &(pgpio->io_invert[i]), comp_id);
            in_cnt++;
        } else
        {
            rtapi_snprintf(buf, HAL_NAME_LEN, "miniemcdrv.%d.pin-out", out_cnt);
            hal_pin_bit_new(buf, HAL_IN, &(pgpio->io_pin[i]), comp_id);
            rtapi_snprintf(buf, HAL_NAME_LEN, "miniemcdrv.%d.pin-out-inv", out_cnt);
            hal_pin_bit_new(buf, HAL_IN, &(pgpio->io_invert[i]), comp_id);
            out_cnt++;
        }
        emcConfigureDefault( i );
    }

	// Trajectory wait output
	rtapi_snprintf(buf, HAL_NAME_LEN, "miniemcdrv.traj-wait-out");
	hal_pin_bit_new(buf, HAL_OUT, &(pgpio->traj_wait), comp_id);
    *(pgpio->traj_wait) = 1;

	// Scaner sync
	rtapi_snprintf(buf, HAL_NAME_LEN, "miniemcdrv.scan-sync-in");
	hal_pin_bit_new(buf, HAL_IN, &(pgpio->scan_sync), comp_id);

    for(i=0; i < num_axis; i++)
    {
      // Check if pin already added
      char contin = 0;
      int j;
      for(j=0; j < i; j++)
        if(axis_map[j] == axis_map[i])
        {
            contin = 1;
            break;
        }
      if(contin) continue;

  	  // commanded position pin
	  rtapi_snprintf(buf, HAL_NAME_LEN, "miniemcdrv.%d.cmd-pos", axis_map[i]);

  	  retval = hal_pin_float_new(buf, HAL_IN, &(pgpio->cmd_pos[axis_map[i]]), comp_id);
	  if (retval != 0)
	  {
        rtapi_app_exit();
		return retval;
	  }
	  //feedback position pin
	  rtapi_snprintf(buf, HAL_NAME_LEN, "miniemcdrv.%d.fb-pos", axis_map[i]);
  	  retval = hal_pin_float_new(buf, HAL_OUT, &(pgpio->fb_pos[axis_map[i]]), comp_id);
	  if (retval != 0)
	  {
        rtapi_app_exit();
		return retval;
	  }
    }



/* export functions */
    retval = hal_export_funct("update-miniemcdrv", update,
                                pgpio, 0, 0, comp_id);
    if (retval != 0)
    {
	rtapi_print_msg(RTAPI_MSG_ERR,
            "miniemcdrv: ERROR: count funct export failed\n");
        rtapi_app_exit();
        return -EIO;
    }

    ioctl(pgpio->fd, SCAN_PIN_SETUP_IOCTL, NULL);

    //emcConfigurePin(11, egpOut );
    //emcSetPin(11, 1 );

    hal_ready(comp_id);
    return 0;
}

void pins_exit()
{
    int i;
    for(i=0; i < num_axis; i++)
    {
        fiq_static.axis[i].configured = 0;
    }
    ioctl(pgpio->fd, AXIS_SET_IOCTL, &fiq_static );

    for( i=0; i < ARR_SZ(GPIOS); i++ )
    {
      emcDeConfigurePin( i );
    }
}

void rtapi_app_exit(void)
{
     pins_exit();
	if( iomem )
		iounmap( iomem, GPIO_MAP_SIZE );
    hal_exit(comp_id);
}

void process_io()
{
    int i;
     for( i=0; i < ARR_SZ(GPIOS); i++ )
    {
        if(  emcGetPinMode( i ) == egpRsv )
            continue;
        if( emcGetPinMode( i ) == egpIn )
        {
            int val = *( pgpio->io_invert[i]) ^ emcGetPin(i);
           *( pgpio->io_pin[i] ) = val == 0 ? 0 : 1;
        } else if( emcGetPinMode( i ) == egpOut )
        {
            int val = *( pgpio->io_pin[i] ) ^ *( pgpio->io_invert[i]);
            emcSetPin( i, val );
        }
    }
    pgpio->pfiq->gpios_changed = 1;
    for( i = 0; i < MAX_PWM; i++)
        emcPWMSetDutyCycle( i ,  *(pgpio->pwm_duty[i]) );
}


/************************************************************************
*            REALTIME  UPDATE FUNCTIONS                                 *
*************************************************************************/

static void update(void *arg, long period)
{
    gpio_t *pgpio = (gpio_t*)arg;
    int		   i;
    static int io_period = 0;
    static int fb_delay[MAX_AXIS] = {0};
    static int pos_err_old[MAX_AXIS] = {0};

//    static int* pTmr = NULL;

//    if( !pTmr )
//        pTmr = mapIoRegister( TIMER_BASE, 0x44 );

    static int update = 0, update_cnt = 0;
 //   if(*(pTmr + TCNTO0) != 0 )
 //       printf("TCON=%x, TCFG0=%x, TCFG1=%x, CNTB=%d, CMPB=%d, TCNTO0=%d\n", *(pTmr + TCON ), *(pTmr + TCFG0 ), *(pTmr + TCFG1 ), *(pTmr + TCNTB0 ), *(pTmr + TCMPB0 ), *(pTmr + TCNTO0) );

    if(pgpio->pfiq->underrun)
    {
        fprintf(stderr, "FIFO underrun!!!\n");
        pgpio->pfiq->underrun = 0;
    }
    // Update IO states only after desired period and if FIFO is half-full
    if (++io_period > io_update_period )
    {
        process_io();
        io_period = 0;
    }
    // If FIFO has maimum size, pause motion controller execution

    if(pgpio->pfiq->mdata.buffsize < fifo_deep)
    {
        *(pgpio->traj_wait) = 1;
    } else
    {
        *(pgpio->traj_wait) = 0;
        return;
    }

    for(i=0; i < num_axis; i++)
    {
        if(axis_map[i] >= 0)
        {
            // Copy actual position to the feedback
          *(pgpio->fb_pos[axis_map[i]]) = *(pgpio->cmd_pos[axis_map[i]]);
            if ( step_pins[i] >0 && dir_pins[i] > 0 )
            {
                long long aux, aux2, dist;

                aux = (long long) (*(pgpio->cmd_pos[axis_map[i]])*1000000.0f);
                // Calculate distance to go at this iteration
                dist = (aux - cmd_pos_prev[i])*(long long)step_per_unit[i]/10000LL;
                // Calculate absolut position from program start
                cmd_pos_accum[i] +=  dist;
                // Set desired position for FIQ stepgen
                pgpio->pfiq->mdata.buffer[pgpio->pfiq->mdata.PutPtr][i].cmd_position = cmd_pos_accum[i]/10000LL;

                //Check if last calculated correction of the position is already appliyed
                if(fb_delay[i] <= 0)
                {
                    // Due to FIQ jitter (cache miss, seems), we need correct position of the stepgen.
                    // Position error is calculated in the FIQ as following: pos_error = cmd_position - step_cnt
                    // Correction applyed by one step per the FIFO cycle
                    // Create dead band if error in range -1..1 to avoid rotor oscilation
                    // Check if already added correction is applied
                        int errDelta = pgpio->pfiq->pos_error[i] - pos_err_old[i];
                    if(errDelta != 0)
                    {
                        if(pgpio->pfiq->pos_error[i] < -1)
                        {
                            dist -= 100LL;
                        }
                        if(pgpio->pfiq->pos_error[i] > 1)
                        {
                            dist += 100LL;
                        }
                        pos_err_old[i] = pgpio->pfiq->pos_error[i];
                    }
                    fb_delay[i] = pgpio->pfiq->mdata.buffsize;
                } else fb_delay[i]--;
                // Calculate DSS adder value
                aux2 = dist * (1LL<<31)/1000000LL;
                // Strore adder and direction values in the FIFO
                if(aux2 > 0)
                {
                    pgpio->pfiq->mdata.buffer[pgpio->pfiq->mdata.PutPtr][i].adder = aux2;
                    pgpio->pfiq->mdata.buffer[pgpio->pfiq->mdata.PutPtr][i].direction = 0;
                } else
                {
                    pgpio->pfiq->mdata.buffer[pgpio->pfiq->mdata.PutPtr][i].adder = - aux2;
                    pgpio->pfiq->mdata.buffer[pgpio->pfiq->mdata.PutPtr][i].direction = 1;
                }
                if( scaner_compat !=0 && i == 0 )
                {
                    if(*(pgpio->scan_sync) != 0)
                        pgpio->pfiq->mdata.buffer[pgpio->pfiq->mdata.PutPtr][i].scan_sync = 1;
                    else
                        pgpio->pfiq->mdata.buffer[pgpio->pfiq->mdata.PutPtr][i].scan_sync = 0;
                }
                // Store last postion
                cmd_pos_prev[i] = aux;
            }
        }
#if 0
        if(update /* && (abs(pgpio->pfiq->pos_error[i]) > 10 )*/ )
        {
                fprintf(stderr, "putPtr=%d, buffsize=%d, step_cnt[%d]=%lld, pos_err[%d]=%lld\n"
                        , pgpio->pfiq->mdata.PutPtr
                        , pgpio->pfiq->mdata.buffsize
                        //, i
                        //, pgpio->pfiq->mdata.buffer[pgpio->pfiq->mdata.PutPtr][i].cmd_position
                        , i
                        , pgpio->pfiq->step_count[i], i, pgpio->pfiq->pos_error[i] );
        }
#endif
    }
#if 0
        update = 0;
        if(++update_cnt >= 500)
        {
 //           printf("scan_sync=%d\n", *(pgpio->scan_sync) );
            update_cnt = 0;
            update = 1;
        };
#endif
    // To avoid simultaneous access to the buffsize, we increment it in the FIQ handler
    pgpio->pfiq->mdata.ringbuff_update = 1;

    // Increment put position
    if(++pgpio->pfiq->mdata.PutPtr  >= fifo_deep)
    {
        pgpio->pfiq->mdata.PutPtr = 0;
    }
}
