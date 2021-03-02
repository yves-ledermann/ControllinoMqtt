#include <Arduino.h>
#include <Controllino.h>
#undef SPI_HAS_EXTENDED_CS_PIN_HANDLING

#include <SPI.h>
#include <PubSubClient.h>
#include "DebugUtils.h"
#include "Configuration.h"
#include "PLC.h"

#define INVALID_VALUE -99


EthernetClient PLC::ethClient;
PubSubClient PLC::mqttClient(ethClient);
Inputs PLC::inputs;
ModbusClient PLC::modbusClient;
uint8_t PLC::validEthernet = 0;

long PLC::mqtt_millis = 0;

void PLC::setup() {
#ifdef CONTROLLINO_MEGA
	DEBUG_PRINT("CONTROLLINO_MEGA defined");
#endif


	DEBUG_PRINT("Initializing PLC");

	Configuration::load();	

	if(Configuration::isValid) {
		PLC::initializeMQTT();
		PLC::initializeEthernet();
		inputs.begin(PLC::onInputChange);
		modbusClient.begin(PLC::onInputChange);
		DEBUG_PRINT("Initialization OK");
	} else {
		INFO_PRINT("PLC not configured");
	}
	DEBUG_PRINT("PLC initialized");

}
 
void PLC::loop() {
	
	Configuration::loop();
	
	if (!validEthernet) {
		if (!Configuration::isConfiguring) {
			PLC::initializeEthernet();
		}
		return;
	}

	if(Configuration::isValid && !Configuration::isConfiguring) {
		bool connected  = true;

		if (!mqttClient.connected() && (mqtt_millis ==0 || ((millis() - mqtt_millis) >= 2000))) {
			mqtt_millis = millis();
			connected = reconnect();
		}

		if (connected) {
			mqttClient.loop();
		}  else {
			INFO_PRINT("Retrying in five seconds");
		}
		modbusClient.loop();
		inputs.loop();
	}
	
}

void PLC::initializeMQTT() {
  INFO_PRINT("Initializing MQTT client...");
  IPAddress server(Configuration::server[0], Configuration::server[1], Configuration::server[2], Configuration::server[3]);
  PLC::mqttClient.setServer(server, Configuration::port);
  PLC::mqttClient.setCallback(PLC::onMQTTMessage);
}

void PLC::initializeEthernet() {
  INFO_PRINT("Initializing Ethernet...");
  IPAddress ip(Configuration::ip[0], Configuration::ip[1], Configuration::ip[2], Configuration::ip[3]);

#ifdef SIMULATED_CONTROLLINO
  Ethernet.init(10);
#endif
  if (Configuration::ip[0] == 0) { // use DHCP
      validEthernet = Ethernet.begin(Configuration::mac);
  } else {
      Ethernet.begin(Configuration::mac, ip);
      validEthernet = 1;
  }

  if (validEthernet == 0) {
    INFO_PRINT("Failed to configure Ethernet using DHCP");
    if (Ethernet.hardwareStatus() == EthernetNoHardware) {
      INFO_PRINT("Ethernet shield was not found.  Sorry, can't run without hardware. :(");
    } else if (Ethernet.linkStatus() == LinkOFF) {
      INFO_PRINT("Ethernet cable is not connected.");
    }
  }
  
  DEBUG_PRINT("Initializing")
  // Allow the hardware to sort itself out
  delay(1500);

  INFO_PRINT_PARAM("Local IP", Ethernet.localIP());
}

bool PLC::reconnect() {
    bool res;
    // Loop until we're reconnected
    INFO_PRINT("Connecting MQTT server...");
    // Attempt to connect

    bool connected = (strlen(Configuration::username) == 0) ?
        mqttClient.connect(Configuration::PLC_Topic) :
        mqttClient.connect(Configuration::PLC_Topic, Configuration::username, Configuration::password);

    if (connected) {
      // Once connected, publish an announcement...
        log("Connected!");
        runDiscovery();
        // ... and resubscribe
		int topicLength = strlen(Configuration::root_Topic) +  strlen(Configuration::PLC_Topic)+4;
		char subscribe_Topic[topicLength]; 
        sprintf_P(subscribe_Topic, PSTR("%s/%s/#"), Configuration::root_Topic, Configuration::PLC_Topic);
        mqttClient.subscribe(subscribe_Topic);
        INFO_PRINT_PARAM("Subscribed to: ", subscribe_Topic);
        res = true;
    } else {
        INFO_PRINT_PARAM("Error!, rc=", mqttClient.state());
        // Wait 5 seconds before retrying
        //delay(5000);
        res = false;
    }
    return res;
}

void PLC::runDiscovery()
{
	char name[8];
	for (int m = 0; m < Configuration::modbus_count; m++) {
		for (int i = 0; i < MODBUS_SIZE; i++) {
			sprintf_P(name, PSTR("M%dI%d"), m + Configuration::modbus_address, i + 1);
			publishEntity(name, false);
		}
	}

	for (int i = 0; i < INPUT_COUNT; i++) {
		publishEntity(Inputs::PIN_NAMES[i], false);
	}

	for (int i = 0; i < RELAY_COUNT; i++) {
		sprintf_P(name, PSTR("R%d"), i);
		publishEntity(name, true);
	}

	for (int i = 0; i < OUTPUT_COUNT; i++) {
		sprintf_P(name, PSTR("D%d"), i);
		publishEntity(name, true);
	}

}

void PLC::publishEntity(const char* name, bool isOutput) {
	char config_msg[250];
	char mac[15];
	char config_topic[50];
	sprintf_P(mac, PSTR("%x%x%x%x%x%x"), Configuration::mac[0], Configuration::mac[1], Configuration::mac[2],
			Configuration::mac[3], Configuration::mac[4], Configuration::mac[5]);

	if (isOutput) {
		sprintf_P(config_msg, PSTR(HASS_DISCOVERY_OUTPUT), Configuration::root_Topic,
				Configuration::PLC_Topic, name, name, mac, name, Configuration::state_Topic, mac);
		sprintf_P(config_topic, PSTR("homeassistant/switch/%s/config"), name);
	} else {
		sprintf_P(config_msg, PSTR(HASS_DISCOVERY_INPUT), name, mac, name, Configuration::root_Topic,
				Configuration::PLC_Topic, name, Configuration::state_Topic, mac);
		sprintf_P(config_topic, PSTR("homeassistant/binary_sensor/%s/config"), name);
	}
	INFO_PRINT_PARAM("Sending ", config_msg);
	mqttClient.publish(config_topic, config_msg, true);
}


void PLC::log(const char* errorMsg)
{
    INFO_PRINT_PARAM("LOG", errorMsg);
    if (mqttClient.connected()) {
      int topicLength = strlen(Configuration::root_Topic) +  strlen(Configuration::log_Topic)+ strlen(Configuration::PLC_Topic)+3;
      char log_Topic[topicLength];
      sprintf_P(log_Topic, PSTR("%s/%s/%s"), Configuration::root_Topic, Configuration::PLC_Topic, Configuration::log_Topic);
      mqttClient.publish(log_Topic, errorMsg);
      INFO_PRINT_PARAM("Published ", errorMsg);
      INFO_PRINT_PARAM("to topic ", log_Topic);
    }
}

void PLC::onInputChange(char* name, uint8_t value) {
    PLC::publish(name, Configuration::state_Topic, value ? ONSTATE : OFFSTATE);
}

void PLC::onMQTTMessage(char* topic, byte* payload, unsigned int length) {
    DEBUG_PRINT_PARAM("Message arrived to topic", topic);
    
    for (unsigned int i=0;i<length;i++) {
        DEBUG_PRINT_PARAM("Payload", (char)payload[i]);
    }
    

    char command[12];
    if (getOuput(topic, command)) {
        DEBUG_PRINT_PARAM("Command: ", command);
        
        int newState = getValue(payload, length);
        
        if (newState != INVALID_VALUE && (command[0]=='R' || command[0]=='D')) {
            updateOutput(command, newState);
        } else if (newState != INVALID_VALUE && command[0]=='M') {
            if (modbusClient.parseCommand(command, newState)) {
                publishEntity(command, true);
                PLC::publish(command, Configuration::state_Topic, newState ? ONSTATE : OFFSTATE);
            }
        }
  } else {
      DEBUG_PRINT("Status message");
  }
}


bool PLC::getOuput(char* topic,char* ouput) {
    String sTopic(topic);
    if(!sTopic.endsWith(Configuration::command_Topic)) {
        return false;
    }
	int topicLength = strlen(Configuration::root_Topic) +  strlen(Configuration::PLC_Topic)+4;
	char subscribe_Topic[topicLength]; 
	sprintf_P(subscribe_Topic, PSTR("%s/%s/#"), Configuration::root_Topic,  Configuration::PLC_Topic);
    String sOutput = sTopic.substring(strlen(subscribe_Topic)-1);
    sOutput = sOutput.substring(0,sOutput.indexOf("/"));
  
    sOutput.toCharArray(ouput, sOutput.length()+1);
    return true;
}

void PLC::updateOutput(char* outputName,int newState) {
    String strOutputName(outputName);
    int outputNumber = strOutputName.substring(1).toInt();

    int pin;

    if (outputName[0] == 'R' && outputNumber>=0 && outputNumber<RELAY_COUNT) {
      pin = CONTROLLINO_R0 + outputNumber;
    } else if (outputName[0] == 'D' && outputNumber >=0 && outputNumber<OUTPUT_PIN_SECTION_1) {
      pin = CONTROLLINO_D0 + outputNumber;
    } else if (outputName[0] == 'D' && outputNumber >=OUTPUT_PIN_SECTION_1 && outputNumber<OUTPUT_PIN_SECTION_2) {
      pin = CONTROLLINO_D12 + outputNumber - OUTPUT_PIN_SECTION_1;
    } else if (outputName[0] == 'D' && outputNumber >=20 && outputNumber< OUTPUT_COUNT) {
      pin = CONTROLLINO_D20 + outputNumber - OUTPUT_PIN_SECTION_2;
    } else {
      log("Invalid output");
      return;
    }
    pinMode(pin,OUTPUT);
    digitalWrite(pin, newState);

    PLC::publish(outputName, Configuration::state_Topic, newState ? ONSTATE : OFFSTATE);
}

void PLC::publish(const char* portName,const char* messageType, const char* payload ){
	// Create message topic
	String topicString = String(Configuration::root_Topic);
	topicString += String("/");
	topicString += String(Configuration::PLC_Topic);
	topicString += String("/");
	topicString += String(portName);
	topicString += String("/");
	topicString += String(messageType);
	
	int topicLength = topicString.length()+1;

	char topic[topicLength];
	topicString.toCharArray(topic, topicLength); 

	mqttClient.publish(topic, payload, true);
}


int PLC::getValue(byte* payload, unsigned int length) {
    if (length==2 && payload[0] == 'O' && payload[1] == 'N') {
        return HIGH;
    } else if (length==3 && payload[0] == 'O' && payload[1] == 'F' && payload[2] == 'F') {
        return LOW;
    } else {
        for (uint16_t i=0; i<length; i++) {
            if (!isdigit(payload[i])) {
                DEBUG_PRINT("Command ignored");
                return INVALID_VALUE;
            }
        }
        char num[length+1];
        memcpy(num, payload, length);
        num[length] = '\0';
        return strtoul(num, NULL, 10);
    }
}
