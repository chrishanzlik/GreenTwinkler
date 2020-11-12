/*
 * GreenTwinkler.c
 *
 * Created: 11.03.2020 17:30:34
 * Author : Christopher Hanzlik
 * Target : ATtiny814
 */ 

#define F_CPU			1250000UL
#define PERIOD			(100)
#define DUTY_MAX		(PERIOD)
#define DUTY_MIN		(5)

#ifdef DEBUG
	#define OPERATION_TIME	15 /* seconds*/
	#define SLEEP_TIME		5  /* seconds*/
#else
	#define OPERATION_TIME	21600L /* seconds (6 hours)  */
	#define SLEEP_TIME		64800L /* seconds (18 hours) */
#endif

#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/sleep.h>
#include <avr/xmega.h>
#include <stdlib.h>
#include <stdbool.h>
#include <util/delay.h>

/******* TYPEDEFS *******/

typedef enum {
	TIME = 0x00,
	BRIGHTNESS = 0x01
} operation_mode;

typedef enum {
	SINKING = 0x00,
	RAISING = 0x01
} pwm_state;

typedef struct {
	volatile register8_t *duty_ptr;
	volatile pwm_state state;
} led_pin;

/******* GLOBALS *******/

uint8_t led_pin_count;
operation_mode mode;
volatile uint16_t RTC_counter_treshold;
volatile led_pin *pins;
volatile bool lights_enabled;

/*
 * Select the internal OSC20 as main clock. Use a prescaler of 16. Results in 1.25 MHz.
 * Choose and setup the external 32kHz crystal.
 */
void init_oscillators(void)
{
	_PROTECTED_WRITE(CLKCTRL.MCLKCTRLA, CLKCTRL_CLKSEL_OSC20M_gc);	
	_PROTECTED_WRITE(CLKCTRL.MCLKCTRLB, CLKCTRL_PEN_bm | CLKCTRL_PDIV_16X_gc);
	_PROTECTED_WRITE(CLKCTRL.MCLKLOCK, CLKCTRL_LOCKEN_bm);
	
	uint8_t XOSC32KCTRLA_temp;

	XOSC32KCTRLA_temp = CLKCTRL.XOSC32KCTRLA;
	XOSC32KCTRLA_temp &= ~CLKCTRL_ENABLE_bm;	// Disable
	_PROTECTED_WRITE(CLKCTRL.XOSC32KCTRLA, XOSC32KCTRLA_temp);

	while(CLKCTRL.MCLKSTATUS & CLKCTRL_XOSC32KS_bm);

	XOSC32KCTRLA_temp = CLKCTRL.XOSC32KCTRLA;
	XOSC32KCTRLA_temp &= ~CLKCTRL_SEL_bm;		// SEL = 0 (Use External Crystal)
	_PROTECTED_WRITE(CLKCTRL.XOSC32KCTRLA, XOSC32KCTRLA_temp);

	XOSC32KCTRLA_temp = CLKCTRL.XOSC32KCTRLA;
	XOSC32KCTRLA_temp |= CLKCTRL_ENABLE_bm;		// Enable
	_PROTECTED_WRITE(CLKCTRL.XOSC32KCTRLA, XOSC32KCTRLA_temp);
}

/*
 * Initialize the RTC clock for waking the MCU from sleep.
 * Using the external 32k crystal with a prescaler of 32k. Results in 1s.
 */
void init_rtc_clock()
{
	while(RTC.STATUS > 0);
	
	RTC.CLKSEL = RTC_CLKSEL_TOSC32K_gc;
	RTC.PITINTCTRL = RTC_PI_bm;
	RTC.PITCTRLA = RTC_PERIOD_CYC32768_gc | RTC_PITEN_bm;
	RTC.DBGCTRL = RTC_DBGRUN_bm;
}

/*
 * Initialize sleep mode with 'POWER DOWN'. sleep_cpu must be called separately.
 */
void init_sleep_mode(void)
{
	SLPCTRL.CTRLA |= SLPCTRL_SMODE_PDOWN_gc;
	SLPCTRL.CTRLA |= SLPCTRL_SEN_bm;
}

/*
 * Initialize PWM by TCA in split mode. Did not enable the timer.
 */
void init_pwm(void)
{
	TCA0.SPLIT.CTRLD = TCA_SPLIT_SPLITM_bm;
	TCA0.SPLIT.CTRLB = TCA_SPLIT_HCMP0EN_bm | TCA_SPLIT_HCMP1EN_bm | TCA_SPLIT_HCMP2EN_bm;
	TCA0.SPLIT.HPER = PERIOD;
	TCA0.SPLIT.CTRLA = TCA_SPLIT_CLKSEL_DIV16_gc;
}

/*
 * Process a duty cycle for given pin. Either it increments or decrements the duty value.
 */
void process_duty(volatile led_pin *pin)
{
	if ((pin->state == RAISING && *pin->duty_ptr >= DUTY_MAX))
	{
		pin->state = SINKING;
		return;
	}
	else if (pin->state == SINKING && *pin->duty_ptr <= DUTY_MIN)
	{
		pin->state = RAISING;
		return;
	}
		
		
	if (pin->state == RAISING)
	{
		(*pin->duty_ptr)++;
	}
	else
	{
		(*pin->duty_ptr)--;
	}
}

/*
 * Enables the lights. TCA is activated.
 */
void enable_lights(void)
{
	lights_enabled = true;
	RTC_counter_treshold = OPERATION_TIME;
	
	uint8_t duty_segment = DUTY_MAX / led_pin_count;
	
	for(int i = 0; i < led_pin_count; i ++) {
		*pins[i].duty_ptr = (duty_segment * (i + 1));
		pins[i].state = RAISING;
	}
	
	TCA0.SPLIT.CTRLA |= TCA_SPLIT_ENABLE_bm;
	TCA0.SPLIT.CTRLB |= TCA_SPLIT_HCMP0EN_bm | TCA_SPLIT_HCMP1EN_bm | TCA_SPLIT_HCMP2EN_bm;
}

/*
 * Disable the lights. TCA is deactivated.
 */
void disable_lights(void)
{
	lights_enabled = false;
	RTC_counter_treshold = SLEEP_TIME;

	TCA0.SPLIT.CTRLB &= ~TCA_SPLIT_HCMP0EN_bm & ~TCA_SPLIT_HCMP1EN_bm & ~TCA_SPLIT_HCMP2EN_bm;
	TCA0.SPLIT.CTRLA &= ~TCA_SPLIT_ENABLE_bm;
}

/*
 * RTC interrupt routine.
 */
ISR(RTC_PIT_vect)
{
	RTC.PITINTFLAGS = RTC_PI_bm;
	
	static volatile uint16_t rounds = 0;
	rounds++;
	
	if (rounds >= RTC_counter_treshold)
	{
		rounds = 0;	
		
		lights_enabled
			? disable_lights()
			: enable_lights();
	}
}

/*
 * Entry point.
 */
int main(void)
{
	PORTA.DIRSET = (1 << 3) | (1 << 4) | (1 << 5);
	
	led_pin_count = 3;
	pins = malloc(sizeof(led_pin) * led_pin_count);
	pins[0].duty_ptr = &TCA0.SPLIT.HCMP0;
	pins[1].duty_ptr = &TCA0.SPLIT.HCMP1;
	pins[2].duty_ptr = &TCA0.SPLIT.HCMP2;
	
	cli();
	init_oscillators();
	init_rtc_clock();
	init_sleep_mode();
	init_pwm();
	enable_lights();	
	sei();
	
    while (1)
    {
		if (lights_enabled)
		{
			for(int i = 0; i < led_pin_count; i++)
			{
				process_duty(&pins[i]);
			}
			_delay_ms(15);
		}
		else
		{
			sleep_cpu();
		}		
    }
}