///////////////////////////////////////////////////////////////////
// M302-HX711eva
//  MIT License
//  Copyright (c) 2023 Masafumi Horimoto
//  Release on 
//  
///////////////////////////////////////////////////////////////////


#include <stdio.h>
#include <SPI.h>
#include <Ethernet2.h>
#include <EthernetUdp2.h> // UDP library from: bjoern@cs.stanford.edu 12/30/2008
#include <avr/pgmspace.h>
#include <avr/wdt.h>
#include <EEPROM.h>
#include "LiquidCrystal_I2C.h"
#include <Wire.h>
#include <Adafruit_I2CDevice.h>
#include <Adafruit_I2CRegister.h>
#include "Adafruit_SHT31.h"
#include "HX711.h"



uint8_t mcusr_mirror __attribute__ ((section (".noinit")));
void get_mcusr(void)     \
  __attribute__((naked)) \
  __attribute__((section(".init3")));
void get_mcusr(void) {
  mcusr_mirror = MCUSR;
  MCUSR = 0;
  wdt_disable();
}

#define  UECS_PORT  16520
#define  pUECSID      0
#define  pMACADDR     6
#define  pCND        0x80
#define  pRADIATION  0xa0
#define  delayMillis 5000UL // 5sec

// HX711 circuit wiring
#define LOADCELL_DOUT_PIN  6
#define LOADCELL_SCK_PIN   7

const char VERSION[16] PROGMEM = "M302-HX711eva06";

char uecsid[6], uecstext[180],strIP[16],linebuf[80];
byte lineptr = 0;
unsigned long cndVal;   // CCM cnd Value
bool      ready,busy;
uint8_t  regs[14];

Adafruit_SHT31 sht31 = Adafruit_SHT31();
HX711 scale;


/////////////////////////////////////
// Hardware Define
/////////////////////////////////////

LiquidCrystal_I2C lcd(0x27,16,2);  // set the LCD address to 0x27 for a 16 chars and 2 line display
char lcdtext[6][17];

byte macaddr[6];
IPAddress localIP,broadcastIP,subnetmaskIP,remoteIP;
EthernetUDP Udp16520; //,Udp16529;
EthernetClient EthClient; // GIS Client

volatile int period1sec = 0;
volatile int period10sec = 0;
volatile int period60sec = 0;

void setup(void) {
  int i;
  const char *ids PROGMEM = "%s:%02X%02X%02X%02X%02X%02X";
  extern void lcdout(int,int,int);
  
  cndVal = 0L;    // Reset cnd value
  lcd.init();
  lcd.backlight();
  configure_wdt();
  EEPROM.get(pUECSID,uecsid);
  EEPROM.get(pMACADDR,macaddr);
  for(i=0;i<16;i++) {
    lcdtext[0][i] = pgm_read_byte(&(VERSION[i]));
  }
  lcdtext[0][i] = 0;
  sprintf(lcdtext[1],ids,"ID",
          uecsid[0],uecsid[1],uecsid[2],uecsid[3],uecsid[4],uecsid[5]);
  lcdout(0,1,1);
  Serial.begin(115200);
  Serial.println(lcdtext[0]);
  delay(500);
  Ethernet.init(10);
  if (Ethernet.begin(macaddr)==0) {
    sprintf(lcdtext[1],"NFL");
  } else {
    localIP = Ethernet.localIP();
    subnetmaskIP = Ethernet.subnetMask();
    for(i=0;i<4;i++) {
      broadcastIP[i] = ~subnetmaskIP[i]|localIP[i];
    }
    sprintf(lcdtext[2],ids,"HW",
            macaddr[0],macaddr[1],macaddr[2],macaddr[3],macaddr[4],macaddr[5]);
    sprintf(strIP,"%d.%d.%d.%d",localIP[0],localIP[1],localIP[2],localIP[3]);
    sprintf(lcdtext[3],"%s",strIP);
    lcdout(2,3,1);
    Udp16520.begin(16520);
  }
  if (! sht31.begin(0x44)) {   // Set to 0x45 for alternate i2c addr
    Serial.println("Couldn't find SHT31");
    lcd.print("NO SHT31");
  }
  sht31.heater(false);

  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);

  wdt_reset();
  cndVal |= 0x00000001;  // Setup completed
  delay(1000);
  //
  // Setup Timer1 Interrupt
  //
  TCCR1A  = 0;
  TCCR1B  = 0;
  TCCR1B |= (1 << WGM12) | (1 << CS12) | (1 << CS10);  //CTCmode //prescaler to 1024
  OCR1A   = 15625-1;
  TIMSK1 |= (1 << OCIE1A);
}

/////////////////////////////////
// Reset Function goto Address 0
/////////////////////////////////
void(*resetFunc)(void) = 0;

extern void lcdout(int,int,int);
extern int setParam(char *);
extern void dumpLowCore(void);
  
#define CCMFMT "<?xml version=\"1.0\"?><UECS ver=\"1.00-E10\"><DATA type=\"%s\" room=\"%d\" region=\"%d\" order=\"%d\" priority=\"%d\">%s</DATA><IP>%s</IP></UECS>";
//char *ids = "%s:%02X%02X%02X%02X%02X%02X";

int dk=0;

void lcd_display_loop(void) {
  dk++;
  switch(dk) {
  case 3:
    lcdout(0,2,1);
    break;
  case 4:
    lcdout(0,3,1);
    break;
  case 5:
    dk = 0;
    lcdout(0,4,1);
    break;
  default:
    lcdout(dk,4,1);
  }
}


/////////////////////////////////
void loop() {
  static int k=0;
  int i,ia,ta,tb,cdsv;
  byte room,region,priority,interval;
  int  order;
  int  inchar ;
  float ther,humi;
  char name[10],dname[11],val[6];

  //  extern void lcdout(int,int,int);
  //  extern int setParam(char *);
  //  extern void dumpLowCore(void);

  char *xmlDT PROGMEM = CCMFMT;
  const char *ids PROGMEM = "%s:%02X%02X%02X%02X%02X%02X";
  
   wdt_reset();
   // 10 sec interval
   if (period10sec==1) {
     UserEvery10Seconds();
     lcd_display_loop();
     period10sec=0;
   }
   // 1 min interval
   if (period60sec==1) {
     period60sec = 0;
     wdt_reset();
   }
   //1 sec interval
   if (period1sec==1) {
      period1sec = 0;
      ia = 0; // cnd
      sprintf(val,"%u",cndVal);
      uecsSendData(ia,xmlDT,val,0);
      cndVal &= 0xfffffffe;            // Clear setup completed flag
      UserEverySecond();
   }
   wdt_reset();
}

ISR(TIMER1_COMPA_vect) {
  static byte cnt10,cnt60;
  cnt10++;
  cnt60++;
  period1sec = 1;
  if (cnt10 >= 10) {
    cnt10 = 0;
    period10sec = 1;
  }
  if (cnt60 >= 60) {
    cnt60 = 0;
    period60sec = 1;
  }
}


void configure_wdt(void) {
  cli();                           // disable interrupts for changing the registers
  MCUSR = 0;                       // reset status register flags
                                   // Put timer in interrupt-only mode:
  WDTCSR |= 0b00011000;            // Set WDCE (5th from left) and WDE (4th from left) to enter config mode,
                                   // using bitwise OR assignment (leaves other bits unchanged).
  WDTCSR =  0b00001000 | 0b100001; // clr WDIE: interrupt enabled
                                   // set WDE: reset disabled
                                   // and set delay interval (right side of bar) to 8 seconds
  sei();                           // re-enable interrupts
                                   // reminder of the definitions for the time before firing
                                   // delay interval patterns:
                                   //  16 ms:     0b000000
                                   //  500 ms:    0b000101
                                   //  1 second:  0b000110
                                   //  2 seconds: 0b000111
                                   //  4 seconds: 0b100000
                                   //  8 seconds: 0b100001
}

void uecsSendData(int id,char *xmlDT,char *val,int z) {
  byte room,region,priority,interval;
  int  order,i,a;
  char name[20],dname[21]; // ,val[6];
  a = id*0x20 + 0x80;
  EEPROM.get(a+0x01,room);
  EEPROM.get(a+0x02,region);
  EEPROM.get(a+0x03,order);
  EEPROM.get(a+0x05,priority);
  EEPROM.get(a+  26,interval);
  EEPROM.get(a+0x06,name);
  for(i=0;i<10;i++) {
    dname[i] = name[i];
    if (name[i]==NULL) break;
  }
  dname[i] = NULL;
  sprintf(uecstext,xmlDT,dname,room,region,order,priority+z,val,strIP);
  Udp16520.beginPacket(broadcastIP,16520);
  Udp16520.write(uecstext);
  Udp16520.endPacket();
}

void UserEverySecond(void) {
  volatile bool aaa;
  volatile byte a=0 ;
  char val[6];
  int ia,l;
  char *xmlDT PROGMEM = CCMFMT;

  cndVal &= 0xfffffffe;            // Clear setup completed flag
  if (aaa) {
    lcd.setCursor(15,0);
    aaa=false;
    lcd.print(">");
  } else {
    lcd.setCursor(15,0);
    aaa=true;
    lcd.print("<");
  }
  wdt_reset();
}

void UserEvery10Seconds(void) {
  extern void lcdout(int,int,int);
  char *xmlDT PROGMEM = CCMFMT;
  char name[10],dtxt[17],tval[11],hval[4];
  int ia,cdsv,l,ti,tc;
  long  w = scale.read_average(10);
  float t = sht31.readTemperature();
  float h = sht31.readHumidity();

  ti = (int)t;
  tc = (int)((float)(t-ti)*100.0);
  
  if (! isnan(t)) {  // check if 'is not a number'
    Serial.print("Temp *C = "); Serial.print(t); Serial.print("\t\t");
  } else { 
    Serial.println("Failed to read temperature");
  }
  sprintf(tval,"%d.%02d",ti,tc);
  uecsSendData(1,xmlDT,tval,0); // InAirTemp
  sprintf(hval,"%d",(int)h);
  uecsSendData(2,xmlDT,hval,0); // InAirHumid
  sprintf(lcdtext[2],"%sC/%s%%",tval,hval);
  ltoa(w,tval,10);
  uecsSendData(3,xmlDT,tval,0); // Weight
  sprintf(lcdtext[3],"W=%s ",tval);
  lcd.setCursor(0,1);
  lcd.print(lcdtext[2]);
  wdt_reset();
}

void UserEveryMinute(void) {
  static byte a=0 ;
  char *xmlDT PROGMEM = CCMFMT;
}

