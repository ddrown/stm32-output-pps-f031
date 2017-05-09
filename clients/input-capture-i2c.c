#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <ctype.h>

#include "i2c.h"
#include "timespec.h"
#include "i2c_registers.h"
#include "adc_calc.h"

// current code assumptions: main channel never stops
static int main_channel = 1, main_channel_hz = 50;

#define AVERAGING_CYCLES 65
#define PPM_INVALID -1000000.0
#define AIM_AFTER_MS 5
#define TCXO_TEMPCOMP_TEMPFILE "/run/.tcxo"
#define TCXO_TEMPCOMP_FILE "/run/tcxo"

// status_flags bitfields
#define STATUS_BAD_CH1        0b1
#define STATUS_BAD_CH2       0b10
#define STATUS_BAD_CH3      0b100
#define STATUS_BAD_CH4     0b1000
// BAD_CHANNEL_STATUS takes 0-indexed channels
#define BAD_CHANNEL_STATUS(x) (1 << x)

static void print_ppm(float ppm) {
  if(ppm < 500 && ppm > -500) {
    printf("%1.3f ", ppm);
  } else {
    printf("- ");
  }
}

static void write_tcxo_ppm(float ppm) {
  FILE *tcxo;

  if(ppm > 500 || ppm < -500) {
    return;
  }

  tcxo = fopen(TCXO_TEMPCOMP_TEMPFILE,"w");
  if(tcxo == NULL) {
    perror("fopen " TCXO_TEMPCOMP_TEMPFILE);
    exit(1);
  }
  fprintf(tcxo, "%1.3f\n", ppm);
  fclose(tcxo);
  rename(TCXO_TEMPCOMP_TEMPFILE, TCXO_TEMPCOMP_FILE);
}

// modifies cycles, first_cycle, last_cycle
static void store_added_offset(double added_offset_ns, struct timespec *cycles, uint16_t *first_cycle, uint16_t *last_cycle) {
  struct timespec *previous_cycle = NULL;
  uint16_t this_cycle_i;

  this_cycle_i = (*last_cycle + 1) % AVERAGING_CYCLES;
  if(*first_cycle != *last_cycle) {
    previous_cycle = &cycles[*last_cycle];
    if(*first_cycle == this_cycle_i) { // we've wrapped around, move the first pointer forward
      *first_cycle = (*first_cycle + 1) % AVERAGING_CYCLES;
    }
  }

  cycles[this_cycle_i].tv_sec = 0;
  cycles[this_cycle_i].tv_nsec = added_offset_ns;
  if(previous_cycle != NULL) {
    add_timespecs(&cycles[this_cycle_i], previous_cycle);
  }

  *last_cycle = this_cycle_i;
}

static float calc_ppm(struct timespec *end, struct timespec *start, uint16_t seconds) {
  double diff_s;
  struct timespec diff;
  diff.tv_sec = end->tv_sec;
  diff.tv_nsec = end->tv_nsec;

  sub_timespecs(&diff, start);
  diff_s = diff.tv_sec + diff.tv_nsec / 1000000000.0;

  float ppm = diff_s * 1000000.0 / seconds;
  return ppm;
}

static uint16_t wrap_add(int16_t a, int16_t b, uint16_t modulus) {
  a = a + b;
  if(a < 0) {
    a += modulus;
  }
  return a;
}

static uint16_t wrap_sub(int16_t a, int16_t b, uint16_t modulus) {
  return wrap_add(a, -1 * b, modulus);
}

static float show_ppm(int16_t number_points, uint16_t last_cycle_index, uint16_t seconds, struct timespec *cycles) {
  float ppm = PPM_INVALID; // default: invalid PPM

  if(number_points >= seconds) {
    uint16_t start_index = wrap_sub(last_cycle_index, seconds, AVERAGING_CYCLES);
    ppm = calc_ppm(&cycles[last_cycle_index], &cycles[start_index], seconds);
    print_ppm(ppm);
  } else {
    printf("- ");
  }

  return ppm;
}

static uint32_t calculate_sleep_ms(uint32_t milliseconds_now, uint32_t milliseconds_irq) {
  uint32_t sleep_ms = 1000 + AIM_AFTER_MS - (milliseconds_now - milliseconds_irq);
  if(sleep_ms > 1000+AIM_AFTER_MS) {
    sleep_ms = 1000+AIM_AFTER_MS;
  } else if(sleep_ms < 1) {
    sleep_ms = 1;
  }
  return sleep_ms;
}

// modifies added_offset_ns, status_flags
static int add_cycles(uint32_t *status_flags, double *added_offset_ns, const struct i2c_registers_type *i2c_registers) {
  static uint32_t previous_cycles[INPUT_CHANNELS] = {0,0,0,0};
  static uint8_t previous_count[INPUT_CHANNELS] = {0,0,0,0};
  static uint32_t primary_ms_last = 0;
  int retval = 1;

  for(uint8_t i = 0; i < INPUT_CHANNELS; i++) {
    int32_t diff;
    
    if(i == main_channel-1) {
      uint32_t milliseconds = i2c_registers->milliseconds_irq_primary - primary_ms_last;
      if(milliseconds < 2000 && milliseconds > 900) {
        diff = i2c_registers->tim2_at_cap[i] - previous_cycles[i] - EXPECTED_FREQ;
        added_offset_ns[i] = diff * 1000000000.0 / EXPECTED_FREQ;
      } else {
        added_offset_ns[i] = 0;
        retval = 0;
	*status_flags |= BAD_CHANNEL_STATUS(i);
      }
      primary_ms_last = i2c_registers->milliseconds_irq_primary;
    } else {
      uint8_t diff_count = i2c_registers->ch_count[i] - previous_count[i];

      if(diff_count == 1 || diff_count == 2) { // treating count as # of seconds, assume 1s/2s=ok, other values=invalid
	diff = i2c_registers->tim2_at_cap[i] - previous_cycles[i] - (EXPECTED_FREQ * diff_count);
	added_offset_ns[i] = diff * 1000000000.0 / (EXPECTED_FREQ * diff_count);
      } else {
	added_offset_ns[i] = 0;
	*status_flags |= BAD_CHANNEL_STATUS(i);
      }
    }

    previous_cycles[i] = i2c_registers->tim2_at_cap[i];
    previous_count[i] = i2c_registers->ch_count[i];
  }

  return retval;
}

// in ppb units
static double tempcomp(const struct tempcomp_data *data) {
  float temp_f = last_temp()*9.0/5.0+32.0;
  return (data->tcxo_a + data->tcxo_b * (temp_f - data->tcxo_d) + data->tcxo_c * pow(temp_f - data->tcxo_d, 2)) * 1000.0;
}

static void poll_i2c(int fd) {
  struct timespec offsets[AVERAGING_CYCLES];
  uint16_t first_cycle_index = 0, last_cycle_index = 0;
  uint32_t last_timestamp = 0;
  struct i2c_registers_type i2c_registers;
  struct i2c_registers_type_page2 i2c_registers_page2;
  struct i2c_registers_type_page3 i2c_registers_page3;
  struct tempcomp_data tempcomp_data;

  memset(offsets, '\0', sizeof(offsets));

  get_i2c_page3(fd, &i2c_registers_page3, &tempcomp_data);

  printf("ts delay status sleepms #pts 1# 1ppm 2# 2ppm 3# 3ppm 4# 4ppm t.offset tempcomp 32s_ppm 64s_ppm ");
  adc_header();
  printf("\n");
  while(1) {
    double added_offset_ns[INPUT_CHANNELS];
    uint32_t sleep_ms;
    uint32_t status_flags = 0;
    int16_t number_points;
    double tempcomp_now;

    get_i2c_structs(fd, &i2c_registers, &i2c_registers_page2);
    add_adc_data(&i2c_registers, &i2c_registers_page2);

    // was there no new data?
    if(i2c_registers.milliseconds_irq_primary == last_timestamp) {
      printf("no new data\n");
      fflush(stdout);
      usleep(995000);
      continue;
    }
    last_timestamp = i2c_registers.milliseconds_irq_primary;

    // aim for 5ms after the event
    sleep_ms = calculate_sleep_ms(i2c_registers.milliseconds_now, i2c_registers.milliseconds_irq_primary);

    if(!add_cycles(&status_flags, added_offset_ns, &i2c_registers)) {
      printf("first cycle, sleeping %d ms\n", sleep_ms);
      fflush(stdout);
      usleep(sleep_ms * 1000);
      continue;
    }

    tempcomp_now = tempcomp(&tempcomp_data);
    for(uint8_t i = 0; i < INPUT_CHANNELS; i++) {
      if(!(status_flags & BAD_CHANNEL_STATUS(i))) {
        added_offset_ns[i] += tempcomp_now; // adjust all channels by the expected tempcomp
      }
    }
    store_added_offset(added_offset_ns[main_channel-1], offsets, &first_cycle_index, &last_cycle_index);

    number_points = wrap_sub(last_cycle_index, first_cycle_index, AVERAGING_CYCLES);

    printf("%lu %2u %3o %4u %2u ",
       time(NULL),
       i2c_registers.milliseconds_now - i2c_registers.milliseconds_irq_primary,
       status_flags,
       sleep_ms,
       number_points
       );
    for(uint8_t i = 0; i < INPUT_CHANNELS; i++) {
//      printf("%10u %3u ", i2c_registers.tim2_at_cap[i], i2c_registers.ch_count[i]);
      printf("%.3f ", added_offset_ns[i]/1000.0);
    }
    print_timespec(&offsets[last_cycle_index]);

    printf(" %3.1f ",tempcomp_now);

    show_ppm(number_points, last_cycle_index, 32, offsets);
    float ppm = show_ppm(number_points, last_cycle_index, AVERAGING_CYCLES-1, offsets);
    write_tcxo_ppm(ppm);

    adc_print();
    printf("\n");
    fflush(stdout);

    usleep(sleep_ms * 1000);
  }
}

void help() {
  fprintf(stderr,"input-capture-i2c -c [primary channel] -z [primary channel hz]\n");
}

int main(int argc, char **argv) {
  int c;
  uint8_t set_page[2];
  uint8_t set_main_channel[3];
  int fd;

  while((c = getopt(argc, argv, "c:z:h")) != -1) {
    switch(c) {
      case 'c':
        main_channel = atoi(optarg);
        break;
      case 'z':
        main_channel_hz = atoi(optarg);
        break;
      case 'h':
        help();
        exit(0);
        break;
      case '?':
        if(optopt == 'c' || optopt == 'z') {
          fprintf(stderr, "Option -%c requires an argument.\n", optopt);
	  exit(1);
        }
        if(isprint(optopt)) {
          fprintf(stderr, "Unknown option -%c\n", optopt);
          exit(1);
        }
        fprintf(stderr, "Unknown option character 0x%x.\n", optopt);
        exit(1);
        break;
      default:
        fprintf(stderr, "getopt is being wierd (0x%x), aborting\n", c);
        exit(1);
        break;
    }
  }

  if(main_channel_hz > 255 || main_channel_hz < 1) {
    printf("invalid value for HZ: %d\n", main_channel_hz);
    exit(1);
  }
  if(main_channel > 4 || main_channel < 1) {
    printf("invalid value for channel: %d\n", main_channel);
    exit(1);
  }

  fd = open_i2c(I2C_ADDR); 

  // primary channel setting is on page1
  set_page[0] = I2C_REGISTER_OFFSET_PAGE;
  set_page[1] = I2C_REGISTER_PAGE1;

  // set the primary channel # and HZ
  set_main_channel[0] = I2C_REGISTER_PRIMARY_CHANNEL;
  set_main_channel[1] = main_channel-1; // channel 1 = 0
  set_main_channel[2] = main_channel_hz;

  lock_i2c(fd);
  write_i2c(fd, set_page, sizeof(set_page));
  write_i2c(fd, set_main_channel, sizeof(set_main_channel));
  unlock_i2c(fd);

  poll_i2c(fd);
}
