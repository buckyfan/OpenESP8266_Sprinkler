// Example code for OpenSprinkler Generation 2

/* This is a program-based sprinkler schedule algorithm.
 Programs are set similar to calendar schedule.
 Each program specifies the days, stations,
 start time, end time, interval and duration.
 The number of programs you can create are subject to EEPROM size.
 
 Creative Commons Attribution-ShareAlike 3.0 license
 Dec 2013 @ Rayshobby.net
 */

/* ==========================================================================================================
   This is a modified version of Rays OpenSprinkler code thats amended to use alternative hardware:
    - Arduino Mega 2560 http://arduino.cc/en/Main/arduinoBoardMega2560 
    - Wiznet W5100 Ethernet with onboard SD Card (this is a fairly common Arduino shield)
    - Freetronics LCD Keypad Shield http://www.freetronics.com/collections/shields/products/lcd-keypad-shield
    - Discrete IO outputs instead of using a shift register (as the Mega2560 has heaps of discretes)
   
   All blocks of code that have been amended for the alternative hardware are marked with:
     <MOD>  at the start of the amended code block and 
     </MOD> at the end of the amended code block

   Version:     Opensprinkler 2.0.7 
   Date:        July 5, 2014
   Repository:

   Refer to the AAA_RELEASE_NOTES file for more information
   
   ========================================================================================================== */

// <MOD> ====== Added libraries for W5100, Freetronics LCD, DS1307 RTC, SD Card =====
#include <Wire.h> 
#include <Time.h>
#include <TimeAlarms.h>
#include <DS1307RTC.h>
#include <MemoryFree.h>
#include <LiquidCrystal.h>
#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>
#include <ICMPPing.h>
#include <tinyFAT.h>
#include <avr/pgmspace.h>
// </MOD> ===== Added libraries for W5100, Freetronics LCD, DS1307 RTC, SD Card =====

#include <limits.h>
#include <OpenSprinklerGen2.h>
//#include <SD.h>
#include <Wire.h>
#include "program.h"

// NTP sync interval (in seconds)
#define NTP_SYNC_INTERVAL       86400L  // 24 hours default
// RC sync interval (in seconds)
#define RTC_SYNC_INTERVAL       60     // 60 seconds default
// Interval for checking network connection (in seconds)
#define CHECK_NETWORK_INTERVAL  15     // 1 minute default
// LCD backlight autodimming timeout
#define LCD_DIMMING_TIMEOUT   30     // 30 seconds default
// Ping test time out (in milliseconds)
#define PING_TIMEOUT            200     // 0.2 second default


// ====== Ethernet defines ======
byte mymac[] = { 0x00,0x69,0x69,0x2D,0x31,0x00 }; // mac address
uint8_t ntpclientportL = 123; // Default NTP client port
int myport;

// <MOD> ====== Added for W5100 & Auto Reboot =====
// byte Ethernet::buffer[ETHER_BUFFER_SIZE];  // Ethernet packet buffer (commented out for W5100)
byte EtherCard::buffer[ETHER_BUFFER_SIZE]; // Ethernet packet buffer
EthernetServer server(STATIC_PORT0);       // Initialize the Ethernet server library
EthernetUDP udp;                           // A UDP instance to let us send and receive packets over UDP
SOCKET pingSocket = 0;                     // Ping socket
ICMPPing ping(pingSocket);                 // Ping object 
// </MOD> ===== Added for W5100 & Auto Reboot =====

char tmp_buffer[TMP_BUFFER_SIZE+1];       // scratch buffer
BufferFiller bfill;                       // buffer filler
unsigned long last_sync_time = 0;

// ====== Object defines ======
OpenSprinkler svc;    // OpenSprinkler object
ProgramData pd;       // ProgramdData object 

// ====== UI defines ======
static char ui_anim_chars[3] = {'.', 'o', 'O'};
  
// poll button press
void button_poll() {

  // read button, if something is pressed, wait till release
  byte button = svc.button_read(BUTTON_WAIT_HOLD);

  if (!(button & BUTTON_FLAG_DOWN)) return;  // repond only to button down events

  svc.button_lasttime = now();
  // button is pressed, turn on LCD right away
  analogWrite(PIN_LCD_BACKLIGHT, 255-svc.options[OPTION_LCD_BACKLIGHT].value); 
  
  switch (button & BUTTON_MASK) {
  case BUTTON_1:
    if (button & BUTTON_FLAG_HOLD) {
      // hold button 1 -> start operation
      svc.enable();
    } 
    else {
      // click button 1 -> display ip address and port number
      svc.lcd_print_ip(ether.myip, ether.hisport);
      delay(DISPLAY_MSG_MS);
    }
    break;

  case BUTTON_2:
    if (button & BUTTON_FLAG_HOLD) {
      // hold button 2 -> disable operation
      svc.disable();
    } 
    else {
      // click button 2 -> display gateway ip address and port number
      svc.lcd_print_ip(ether.gwip, 0);
      delay(DISPLAY_MSG_MS);
    }
    break;

  case BUTTON_3:
    if (button & BUTTON_FLAG_HOLD) {
      // hold button 3 -> reboot
      svc.button_read(BUTTON_WAIT_RELEASE);
      svc.reboot();
    } 
    else {
      // click button 3 -> switch board display (cycle through master and all extension boards)
      svc.status.display_board = (svc.status.display_board + 1) % (svc.nboards);
    }
    break;
  }
}

// ======================
// Arduino Setup Function
// ======================
void setup() { 
  //Serial.begin(9600);
  //Serial.println("start");
  svc.begin();          // OpenSprinkler init
  svc.options_setup();  // Setup options
 
  pd.init();            // ProgramData init
  // calculate http port number
  myport = (int)(svc.options[OPTION_HTTPPORT_1].value<<8) + (int)svc.options[OPTION_HTTPPORT_0].value;

  setSyncInterval(RTC_SYNC_INTERVAL);  // RTC sync interval
  // if rtc exists, sets it as time sync source
  setSyncProvider(svc.status.has_rtc ? RTC.get : NULL);
  delay(500);
  svc.lcd_print_time(0);  // display time to LCD
  
  // attempt to detect SD card
  svc.lcd_print_line_clear_pgm(PSTR("Detecting uSD..."), 1);
  
  byte res=file.initFAT(0);  // initialize wipriceth default SPI speed
  if (res==NO_ERROR) {
    svc.status.has_sd = 1;
  }

  svc.lcd_print_line_clear_pgm(PSTR("Connecting..."), 1);
    
  if (svc.start_network(mymac, myport)) {  // initialize network
    svc.status.network_fails = 0;
  } else  svc.status.network_fails = 1;

  delay(500);

  svc.apply_all_station_bits(); // reset station bits
  
  // <MOD> ====== Added for Auto Reboot =====
  // wdt_enable(WDTO_4S);  // enabled watchdog timer    
  if(AUTO_REBOOT)
      Alarm.alarmRepeat(REBOOT_HR,REBOOT_MIN,REBOOT_SEC, svc.reboot);      
  // </MOD> ===== Added for Auto Reboot ===== 
  
  svc.button_lasttime = now();
}

// =================
// Arduino Main Loop
// =================
void loop()
{
  static unsigned long last_time = 0;
  static unsigned long last_minute = 0;
  static uint16_t pos;

  byte bid, sid, s, pid, seq, bitvalue, mas;
  ProgramStruct prog;

  seq = svc.options[OPTION_SEQUENTIAL].value;
  mas = svc.options[OPTION_MASTER_STATION].value;
  //wdt_reset();  // reset watchdog timer

  // ====== Process Ethernet packets ======
  pos=ether.packetLoop(ether.packetReceive());
  if (pos>0) {  // packet received

    // <MOD> ====== Added for W5100 =====
    // analyze_get_url((char*)Ethernet::buffer+pos);  // (commented out for W5100)
    analyze_get_url((char*)EtherCard::buffer+pos);
    // </MOD> ===== Added for W5100 =====

  }
  // ======================================
 
  button_poll();    // process button press


  // if 1 second has passed
  time_t curr_time = now();
  if (last_time != curr_time) {

    last_time = curr_time;
    svc.lcd_print_time(0);       // print time

    // ====== Check raindelay status ======
    if (svc.status.rain_delayed) {
      if (curr_time >= svc.raindelay_stop_time) {
        // raindelay time is over      
        svc.raindelay_stop();
      }
    } else {
      if (svc.raindelay_stop_time > curr_time) {
        svc.raindelay_start();
      }
    }
    
    // ====== Check LCD backlight timeout ======
    if (svc.button_lasttime != 0 && svc.button_lasttime + LCD_DIMMING_TIMEOUT < curr_time) {
      analogWrite(PIN_LCD_BACKLIGHT, 255-svc.options[OPTION_LCD_DIMMING].value); 
      svc.button_lasttime = 0;
    }
    
    // ====== Check rain sensor status ======
    svc.rainsensor_status();    

    // ====== Schedule program data ======
    // Check if we are cleared to schedule a new program. The conditions are:
    // 1) the controller is in program mode (manual_mode == 0), and if
    // 2) either the controller is not busy or is in concurrent mode
    if (svc.status.manual_mode==0 && (svc.status.program_busy==0 || seq==0)) {
      unsigned long curr_minute = curr_time / 60;
      boolean match_found = false;
      // since the granularity of start time is minute
      // we only need to check once every minute
      if (curr_minute != last_minute) {
        last_minute = curr_minute;
        // check through all programs
        for(pid=0; pid<pd.nprograms; pid++) {
          pd.read(pid, &prog);
          if(prog.check_match(curr_time) && prog.duration != 0) {
            // program match found
            // process all selected stations
            for(bid=0; bid<svc.nboards; bid++) {
              for(s=0;s<8;s++) {
                sid=bid*8+s;
                // ignore master station because it's not scheduled independently
                if (mas == sid+1)  continue;
                // if the station is current running, skip it
                if (svc.station_bits[bid]&(1<<s)) continue;
                
                // if station bits match
                if(prog.stations[bid]&(1<<s)) {
                  // initialize schedule data
                  // store duration temporarily in stop_time variable
                  // duration is scaled by water level
                  pd.scheduled_stop_time[sid] = (unsigned long)prog.duration * svc.options[OPTION_WATER_PERCENTAGE].value / 100;
                  pd.scheduled_program_index[sid] = pid+1;
                  match_found = true;
                }
              }
            }
          }
        }
        
        // calculate start and end time
        if (match_found) {
          schedule_all_stations(curr_time, seq);
        }
      }//if_check_current_minute
    } //if_cleared_for_scheduling
    
    // ====== Run program data ======
    // Check if a program is running currently
    if (svc.status.program_busy){
      for(bid=0;bid<svc.nboards; bid++) {
        bitvalue = svc.station_bits[bid];
        for(s=0;s<8;s++) {
          byte sid = bid*8+s;
          
          // check if the current station has a scheduled program
          // this includes running stations and stations waiting to run
          if (pd.scheduled_program_index[sid] > 0) {
            // if so, check if we should turn it off
            if (curr_time >= pd.scheduled_stop_time[sid])
            {
              svc.set_station_bit(sid, 0);

              // record lastrun log (only for non-master stations)
              if(mas != sid+1)
              {
                pd.lastrun.station = sid;
                pd.lastrun.program = pd.scheduled_program_index[sid];
                pd.lastrun.duration = curr_time - pd.scheduled_start_time[sid];
                pd.lastrun.endtime = curr_time;
                write_log();
              }      
              
              // reset program data variables
              //pd.remaining_time[sid] = 0;
              pd.scheduled_start_time[sid] = 0;
              pd.scheduled_stop_time[sid] = 0;
              pd.scheduled_program_index[sid] = 0;            
            }
          }
          // if current station is not running, check if we should turn it on
          if(!((bitvalue>>s)&1)) {
            if (curr_time >= pd.scheduled_start_time[sid] && curr_time < pd.scheduled_stop_time[sid]) {
              svc.set_station_bit(sid, 1);
              
              // schedule master station here if
              // 1) master station is defined
              // 2) the station is non-master and is set to activate master
              // 3) controller is not running in manual mode AND sequential is true
              if ((mas>0) && (mas!=sid+1) && (svc.masop_bits[bid]&(1<<s)) && seq && svc.status.manual_mode==0) {
                byte masid=mas-1;
                // master will turn on when a station opens,
                // adjusted by the master on and off time
                pd.scheduled_start_time[masid] = pd.scheduled_start_time[sid]+svc.options[OPTION_MASTER_ON_ADJ].value;
                pd.scheduled_stop_time[masid] = pd.scheduled_stop_time[sid]+svc.options[OPTION_MASTER_OFF_ADJ].value-60;
                pd.scheduled_program_index[masid] = pd.scheduled_program_index[sid];
                // check if we should turn master on now
                if (curr_time >= pd.scheduled_start_time[masid] && curr_time < pd.scheduled_stop_time[masid])
                {
                  svc.set_station_bit(masid, 1);
                }
              }
            }
          }
        }//end_s
      }//end_bid
      
      // process dynamic events
      process_dynamic_events();
      
      // activate/deactivate valves
      svc.apply_all_station_bits();

      boolean program_still_busy = false;
      for(sid=0;sid<svc.nstations;sid++) {
        // check if any station has a non-zero and non-infinity stop time
        if (pd.scheduled_stop_time[sid] > 0 && pd.scheduled_stop_time[sid] < ULONG_MAX) {
          program_still_busy = true;
          break;
        }
      }
      // if the program is finished, reset program busy bit
      if (program_still_busy == false) {
        // turn off all stations
        svc.clear_all_station_bits();
        
        svc.status.program_busy = 0;
        
        // in case some options have changed while executing the program        
        mas = svc.options[OPTION_MASTER_STATION].value; // update master station
      }
      
    }//if_some_program_is_running

    // handle master station for manual or parallel mode
    if ((mas>0) && svc.status.manual_mode==1 || seq==0) {
      // in parallel mode or manual mode
      // master will remain on until the end of program
      byte masbit = 0;
      for(sid=0;sid<svc.nstations;sid++) {
        bid = sid>>3;
        s = sid&0x07;
        // check there is any non-master station that activates master and is currently turned on
        if ((mas!=sid+1) && (svc.station_bits[bid]&(1<<s)) && (svc.masop_bits[bid]&(1<<s))) {
          masbit = 1;
          break;
        }
      }
      svc.set_station_bit(mas-1, masbit);
    }    
    
    // process dynamic events
    process_dynamic_events();
      
          
    // activate/deactivate valves
    svc.apply_all_station_bits();
    
    // process LCD display
    svc.lcd_print_station(1, ui_anim_chars[curr_time%3]);
    
    // check network connection
    check_network(curr_time);
    
    // perform ntp sync
    perform_ntp_sync(curr_time);
  }
}

void manual_station_off(byte sid) {
  unsigned long curr_time = now();

  // set station stop time (now)
  pd.scheduled_stop_time[sid] = curr_time;  
}

void manual_station_on(byte sid, int ontimer) {
  unsigned long curr_time = now();
  // set station start time (now)
  pd.scheduled_start_time[sid] = curr_time + 1;
  if (ontimer == 0) {
    pd.scheduled_stop_time[sid] = ULONG_MAX-1;
  } else { 
    pd.scheduled_stop_time[sid] = pd.scheduled_start_time[sid] + ontimer;
  }
  // set program index
  pd.scheduled_program_index[sid] = 99;
  svc.status.program_busy = 1;
}

void perform_ntp_sync(time_t curr_time) {
  // do not perform sync if this option is disabled, or if network is not available, or if a program is running
  if (svc.options[OPTION_USE_NTP].value==0 || svc.status.network_fails>0 || svc.status.program_busy) return;   
  // sync every 24 hour
  if (last_sync_time == 0 || (curr_time - last_sync_time > NTP_SYNC_INTERVAL)) {
    last_sync_time = curr_time;
    svc.lcd_print_line_clear_pgm(PSTR("NTP Syncing..."),1);
    unsigned long t = getNtpTime();   
    if (t>0) {    
      setTime(t);
      if (svc.status.has_rtc) RTC.set(t); // if rtc exists, update rtc
    }
  }
}

void check_network(time_t curr_time) {
  static unsigned long last_check_time = 0;

  if (last_check_time == 0) {last_check_time = curr_time; return;}
  // check network condition periodically
  // check interval depends on the fail times
  // the more time it fails, the longer the gap between two checks
  unsigned long interval = 1 << (svc.status.network_fails);
  interval *= CHECK_NETWORK_INTERVAL;
  
// <MOD> ====== Added for W5100 =====
  
/*  
  if (curr_time - last_check_time > interval) {
    // change LCD icon to indicate it's checking network
    svc.lcd.setCursor(15, 1);
    svc.lcd.write(4);
      
    last_check_time = curr_time;
   
    // ping gateway ip
    ether.clientIcmpRequest(ether.gwip);
    
    unsigned long start = millis();
    boolean failed = true;
    // wait at most PING_TIMEOUT milliseconds for ping result
    do {
      ether.packetLoop(ether.packetReceive());
      if (ether.packetLoopIcmpCheckReply(ether.gwip)) {
        failed = false;
        break;
      }
    } while(millis() - start < PING_TIMEOUT);
    if (failed)  {
      svc.status.network_fails++;
      // clamp it to 6
      if (svc.status.network_fails > 6) svc.status.network_fails = 6;
    }
    else svc.status.network_fails=0;
    // if failed more than once, reconnect
    if (svc.status.network_fails>2&&svc.options[OPTION_NETFAIL_RECONNECT].value) {
      svc.lcd_print_line_clear_pgm(PSTR("Reconnecting..."),0);
      if (svc.start_network(mymac, myport))
        svc.status.network_fails=0;
    }
  }
    */
  // check network condition periodically
  if (curr_time - last_check_time > interval) 
  {
    // change LCD icon to indicate it's checking network
    svc.lcd.setCursor(15, 1);
    svc.lcd.write(4);
    
    last_check_time = curr_time;

    // ping gateway ip  
    boolean result = ping(2, ether.gwip, tmp_buffer);

    if (result == false)
    {
      svc.status.network_fails++;
      // clamp it to 6
      if (svc.status.network_fails > 6) svc.status.network_fails = 6;
    }
    else
      svc.status.network_fails=0;

    // if failed more than once, reconnect
    if (svc.status.network_fails>2&&svc.options[OPTION_NETFAIL_RECONNECT].value) {
      svc.lcd_print_line_clear_pgm(PSTR("Reconnecting..."),0);
      if (svc.start_network(mymac, myport))
        svc.status.network_fails=0;
    }
  }
  // </MOD> ===== Added for W5100 =====    
}

void process_dynamic_events()
{
  // check if rain is detected
  byte mas = svc.options[OPTION_MASTER_STATION].value;  
  bool rain = false;
  bool en = svc.status.enabled ? true : false;
  bool mm = svc.status.manual_mode ? true : false;
  if (svc.status.rain_delayed || (svc.options[OPTION_USE_RAINSENSOR].value && svc.status.rain_sensed)) {
    rain = true;
  }
  unsigned long curr_time = now();

  byte sid, s, bid, rbits, sbits;
  for(bid=0;bid<svc.nboards;bid++) {
    rbits = svc.ignrain_bits[bid];
    sbits = svc.station_bits[bid];
    for(s=0;s<8;s++) {
      sid=bid*8+s;
      // if the controller is in program mode (not manual mode)
      // and this is a normal program (not a run-once program)
      // and either the controller is disabled, or
      // if raining and ignore rain bit is cleared
      if (!mm && (pd.scheduled_program_index[sid] != 254) &&
          (!en || (rain && !(rbits&(1<<s)))) ) {
        if (sbits&(1<<s)) { // if station is currently running
          // stop the station immediately
          svc.set_station_bit(sid, 0);

          // record lastrun log (only for non-master stations)
          if(mas != sid+1)
          {
            pd.lastrun.station = sid;
            pd.lastrun.program = pd.scheduled_program_index[sid];
            pd.lastrun.duration = curr_time - pd.scheduled_start_time[sid];
            pd.lastrun.endtime = curr_time;
            write_log();
          }      
          
          // reset program data variables
          //pd.remaining_time[sid] = 0;
          pd.scheduled_start_time[sid] = 0;
          pd.scheduled_stop_time[sid] = 0;
          pd.scheduled_program_index[sid] = 0;               
        } else if (pd.scheduled_program_index[sid] > 0) { // if station is currently not running but is waiting to run
          // reset program data variables
          pd.scheduled_start_time[sid] = 0;
          pd.scheduled_stop_time[sid] = 0;
          pd.scheduled_program_index[sid] = 0;             
        }
      }
    }
  }      
}

void schedule_all_stations(unsigned long curr_time, byte seq)
{
  unsigned long accumulate_time = curr_time + 1;
  byte sid;
    
  // calculate start time of each station
	if (seq) {
		// in sequential mode
	  // stations run one after another
  	// separated by station delay time

    for(sid=0;sid<svc.nstations;sid++) {
      if(pd.scheduled_stop_time[sid]) {
        pd.scheduled_start_time[sid] = accumulate_time;
        accumulate_time += pd.scheduled_stop_time[sid];
        pd.scheduled_stop_time[sid] = accumulate_time;
        accumulate_time += svc.options[OPTION_STATION_DELAY_TIME].value; // add station delay time
        svc.status.program_busy = 1;  // set program busy bit
		  }
		}
	} else {
		// in concurrent mode, stations are allowed to run in parallel
    for(sid=0;sid<svc.nstations;sid++) {
      byte bid=sid/8;
      byte s=sid%8;
      if(pd.scheduled_stop_time[sid] && !(svc.station_bits[bid]&(1<<s))) {
        pd.scheduled_start_time[sid] = accumulate_time;
        pd.scheduled_stop_time[sid] = accumulate_time + pd.scheduled_stop_time[sid];
        svc.status.program_busy = 1;  // set program busy bit
      }
    }
	}
}

void reset_all_stations() {
  svc.clear_all_station_bits();
  svc.apply_all_station_bits();
  pd.reset_runtime();
}

void delete_log(char *name) {
  if (!svc.status.has_sd) return;

  strcat(name, ".txt");
  
  if (!file.exists(name))  return;
  
  file.delFile(name);
  file.closeFile();
}

// write lastrun record to log on SD card
void write_log() {
  if (!svc.status.has_sd)  return;

  // file name will be xxxxx.log where xxxxx is the day in epoch time
  ultoa(pd.lastrun.endtime / 86400, tmp_buffer, 10);
  strcat(tmp_buffer, ".txt");
  
  //Serial.println(tmp_buffer);
  if (!file.exists(tmp_buffer)) {
    // file does not exist yet, create now
    if (!file.create(tmp_buffer)) return;
  }

  file.openFile(tmp_buffer, FILEMODE_TEXT_WRITE);

  tmp_buffer[0] = 0;
  strcat(tmp_buffer, "[");
  
  char p[12];
  
  itoa(pd.lastrun.program, p, 10);
  strcat(tmp_buffer, p);
  strcat(tmp_buffer, ",");
    
  itoa(pd.lastrun.station, p, 10);
  strcat(tmp_buffer, p);
  strcat(tmp_buffer, ",");

  itoa(pd.lastrun.duration, p, 10);  
  strcat(tmp_buffer, p);
  strcat(tmp_buffer, ",");

  ultoa(pd.lastrun.endtime, p, 10);  
  strcat(tmp_buffer, p);
  strcat(tmp_buffer, "]");

  file.writeLn(tmp_buffer);
  //Serial.println(tmp_buffer);
  
  file.closeFile();
}
