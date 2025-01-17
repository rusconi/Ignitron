/*
 * SparkDataControl.cpp
 *
 *  Created on: 19.08.2021
 *      Author: stangreg
 */

#include "SparkDataControl.h"

SparkBLEControl *SparkDataControl::bleControl = nullptr;
SparkStreamReader SparkDataControl::spark_ssr;
SparkMessage SparkDataControl::spark_msg;
SparkPresetBuilder SparkDataControl::presetBuilder;
SparkDisplayControl *SparkDataControl::spark_display = nullptr;
SparkKeyboardControl *SparkDataControl::keyboardControl;

queue<ByteVector> SparkDataControl::msgQueue;

Preset SparkDataControl::activePreset_;
Preset SparkDataControl::pendingPreset_ = activePreset_;

int SparkDataControl::activeBank_ = 0;
int SparkDataControl::pendingBank_ = 0;
Preset SparkDataControl::appReceivedPreset_;
int SparkDataControl::presetEditMode_ = PRESET_EDIT_NONE;

int SparkDataControl::presetNumToEdit_ = 0;
int SparkDataControl::presetBankToEdit_ = 0;


int SparkDataControl::activePresetNum_ = 1;
string SparkDataControl::responseMsg_ = "";
byte SparkDataControl::nextMessageNum = 0x01;

vector<ByteVector> SparkDataControl::ack_msg;
vector<ByteVector> SparkDataControl::current_msg;

bool SparkDataControl::customPresetAckPending = false;
bool SparkDataControl::retrieveCurrentPreset = false;
bool SparkDataControl::customPresetNumberChangePending = false;
int SparkDataControl::operationMode_ = SPARK_MODE_APP;

int SparkDataControl::currentBTMode_ = BT_MODE_BLE;
int SparkDataControl::sparkModeAmp = SPARK_MODE_AMP;
int SparkDataControl::sparkModeApp = SPARK_MODE_APP;
int SparkDataControl::sparkAmpType = AMP_TYPE_40;
string SparkDataControl::sparkAmpName = "Spark 40";
bool SparkDataControl::with_delay = false;

SparkDataControl::SparkDataControl() {
	//init();
	bleControl = new SparkBLEControl(this);
	keyboardControl = new SparkKeyboardControl();
	keyboardControl->init();
}

SparkDataControl::~SparkDataControl() {
	if (bleControl)
		delete bleControl;
	if (spark_display)
		delete spark_display;
	if (keyboardControl)
		delete keyboardControl;
}

int SparkDataControl::init(int opModeInput) {
	operationMode_ = opModeInput;

	string currentSparkModeFile;
	int sparkModeInput = 0;
	// Creating vector of presets
	presetBuilder.initializePresetListFromFS();

	fileSystem.openFromFile(sparkModeFileName.c_str(), currentSparkModeFile);

	stringstream sparkModeStream(currentSparkModeFile);
	string line;
	string currentBTModeFile;
	stringstream btModeStream;

	while (getline(sparkModeStream, line)) {
		sparkModeInput = (int)(line[0]-'0'); // was: stoi(line);
	}
	if (sparkModeInput != 0) {
		operationMode_ = sparkModeInput;
		//Serial.printf("Reading operation mode from file: %d.", sparkModeInput);
	}

	// Define MAC address required for keyboard
	uint8_t mac_keyboard[] = { 0xB4, 0xE6, 0x2D, 0xB2, 0x1B, 0x36 }; //{0x36, 0x33, 0x33, 0x33, 0x33, 0x33};


	switch(operationMode_){
	case SPARK_MODE_APP:
		// Set MAC address for BLE keyboard
		esp_base_mac_addr_set(&mac_keyboard[0]);

		// initialize BLE
		bleKeyboard.setName("Ignitron BLE");
		bleKeyboard.begin();
		//delay(2000);
		bleKeyboard.end();
		bleControl->initBLE(&bleNotificationCallback);
		break;
	case SPARK_MODE_AMP:
		pendingBank_ = 1;
		activeBank_ = 1;
		fileSystem.openFromFile(btModeFileName.c_str(), currentBTModeFile);
		btModeStream.str(currentBTModeFile);

		while (getline(btModeStream, line)) {
			currentBTMode_ = (int)(line[0]-'0'); // was: stoi(line);
		}

		if (currentBTMode_ == BT_MODE_BLE) {
			bleControl->startServer();
		} else if (currentBTMode_ == BT_MODE_SERIAL) {
			bleControl->startBTSerial();
		}
		activePreset_ = presetBuilder.getPreset(activePresetNum_, activeBank_);
		pendingPreset_ = presetBuilder.getPreset(activePresetNum_,
				pendingBank_);
		break;
	case SPARK_MODE_KEYBOARD:
		// Set MAC address for BLE keyboard
		esp_base_mac_addr_set(&mac_keyboard[0]);

		// initialize BLE
		bleKeyboard.setName("Ignitron BLE");
		bleKeyboard.begin();
		break;
	}

	return operationMode_;

}


void SparkDataControl::switchOperationMode(int opMode) {
	operationMode_ = opMode;
	buttonMode_ = SWITCH_MODE_PRESET;
	if (opMode == SPARK_MODE_APP) {
		bleKeyboard.end();
	} else if (opMode == SPARK_MODE_LOOPER) {
		bleKeyboard.start();
	}
	updatePendingWithActive();
}

void SparkDataControl::setDisplayControl(SparkDisplayControl *display) {
	spark_display = display;
}

void SparkDataControl::checkForUpdates() {

	ByteVector currentMsg;
	while(msgQueue.size() > 0){
		currentMsg = msgQueue.front();
		processSparkData(currentMsg);
		msgQueue.pop();
	}

	if (spark_ssr.isPresetNumberUpdated() && (operationMode_ == SPARK_MODE_APP || operationMode_ == SPARK_MODE_LOOPER)) {
		if (pendingBank_ == 0) {
			DEBUG_PRINTLN("Preset number has changed, getting current preset from Spark");
			getCurrentPresetFromSpark();
		}
		spark_ssr.resetPresetNumberUpdateFlag();
	}

	// Check if active preset has been updated
	// If so, update the preset variables
	if (spark_ssr.isPresetUpdated() && (operationMode_ == SPARK_MODE_APP || operationMode_ == SPARK_MODE_LOOPER)){
		pendingPreset_  = spark_ssr.currentSetting();
		activePreset_ = pendingPreset_;
		spark_ssr.resetPresetUpdateFlag();
	}

	if (operationMode_ == SPARK_MODE_AMP) {

		// Read incoming (serial) Bluetooth data, if available
		while (bleControl && bleControl->byteAvailable()) {
			byte inputByte = bleControl->readByte();
			currentBTMsg.push_back(inputByte);
			int msgSize = currentBTMsg.size();
			if (msgSize > 0) {
				if (currentBTMsg[msgSize - 1] == 0xF7) {
					//DEBUG_PRINTF("Free Heap size: %d\n", ESP.getFreeHeap()); DEBUG_PRINTF("Max free Heap block: %d\n",
					//		ESP.getMaxAllocHeap());

					DEBUG_PRINTLN("Received a message");
					DEBUG_PRINTVECTOR(currentBTMsg);
					DEBUG_PRINTLN();
					//heap_caps_check_integrity_all(true) ;
					processSparkData(currentBTMsg);

					currentBTMsg.clear();

				}
			}
		}

	}

}

void SparkDataControl::startBLEServer() {
	bleControl->startServer();
}

bool SparkDataControl::checkBLEConnection() {
	if (bleControl->isAmpConnected()) {
		return true;
	}
	if (bleControl->isConnectionFound()) {
		if (bleControl->connectToServer()) {
			bleControl->subscribeToNotifications(&bleNotificationCallback);
			Serial.println("BLE connection to Spark established.");
			return true;
		} else {
			Serial.println("Failed to connect, starting scan");
			bleControl->startScan();
			return false;
		}
	}
	return false;
}


Preset SparkDataControl::getPreset(int bank, int pre) {
	return presetBuilder.getPreset(bank, pre);
}

int SparkDataControl::getNumberOfBanks() {
	return presetBuilder.getNumberOfBanks();
}

bool SparkDataControl::isAmpConnected() {
	return bleControl->isAmpConnected();
}

bool SparkDataControl::isAppConnected() {
	return bleControl->isAppConnected();
}

void SparkDataControl::bleNotificationCallback(
		NimBLERemoteCharacteristic *pRemoteCharacteristic, uint8_t *pData,
		size_t length, bool isNotify) {

	//Triggered when data is received from Spark Amp in APP mode
	// Transform data into ByteVetor and process
	ByteVector chunk(&pData[0], &pData[length]);
	DEBUG_PRINT("Incoming block: ");
	DEBUG_PRINTVECTOR(chunk);
	DEBUG_PRINTLN();
	// Add incoming data to message queue for processing
	msgQueue.push(chunk);

}

void SparkDataControl::processSparkData(ByteVector blk) {

	// Check if incoming message requires sending an acknowledgment
	handleSendingAck(blk);

	int retCode = spark_ssr.processBlock(blk);
	if (retCode == MSG_PROCESS_RES_REQUEST && operationMode_ == SPARK_MODE_AMP) {
		handleAmpModeRequest();
	}
	if (retCode == MSG_PROCESS_RES_COMPLETE) {
		handleAppModeResponse();
	}

	handleIncomingAck();

}

bool SparkDataControl::getCurrentPresetFromSpark() {
	int hw_preset = -1;
	if (pendingBank_ == 0) {
		hw_preset = activePresetNum_;
	}
	current_msg = spark_msg.get_current_preset(nextMessageNum, hw_preset);
	DEBUG_PRINTLN("Getting current preset from Spark");

	return sendMessageToBT(current_msg);
}

void SparkDataControl::updatePendingPreset(int bnk) {
	pendingPreset_ = getPreset(bnk, activePresetNum_);
}

void SparkDataControl::updatePendingWithActive() {
	pendingBank_ = activeBank_;
	pendingPreset_ = activePreset_;
}
bool SparkDataControl::switchPreset(int pre, bool isInitial) {

	bool retValue = false;

	int bnk = pendingBank_;
	if (operationMode_ == SPARK_MODE_APP || operationMode_ == SPARK_MODE_LOOPER) {
		// Check if selected preset is equal to active preset.
		// In this case toggle the drive pedal only
		if (pre == activePresetNum_ && activeBank_ == pendingBank_
				&& !(activePreset_.isEmpty)
				&& !isInitial) {

			retValue = toggleEffect(INDEX_FX_DRIVE);
		}
		// Preset is actually changing
		else {
			// Switch HW preset
			if (bnk == 0) {
				Serial.printf("Changing to HW preset %d...", pre);
				current_msg = spark_msg.change_hardware_preset(nextMessageNum, pre);
				retValue = sendMessageToBT(current_msg);
			}
			// Switch to custom preset
			else {
				pendingPreset_ = presetBuilder.getPreset(bnk, pre);
				if (pendingPreset_.isEmpty) {
					Serial.println("Empty preset, skipping further processing");
					return false;
				}

				current_msg = spark_msg.create_preset(pendingPreset_, DIR_TO_SPARK, nextMessageNum);
				Serial.printf("Changing to preset %02d-%d...", bnk, pre);
				if (sendMessageToBT(current_msg)) {
					customPresetNumberChangePending = true;
					retValue = true;
				}
			} // Else (custom preset)
		} // else (preset changing)
	} // if APP / LOOPER mode
	if (retValue == true) {
		activeBank_ = bnk;
		activePresetNum_ = pre;
	}

	return retValue;
}

bool SparkDataControl::switchEffectOnOff(string fx_name, bool enable) {

	Serial.printf("Switching %s effect %s...", enable ? "On" : "Off",
			fx_name.c_str());
	for (int i = 0; i < pendingPreset_.pedals.size(); i++) {
		Pedal currentPedal = pendingPreset_.pedals[i];
		if (currentPedal.name == fx_name) {
			pendingPreset_.pedals[i].isOn = enable;
			break;
		}
	}
	current_msg = spark_msg.turn_effect_onoff(nextMessageNum, fx_name, enable);

	return sendMessageToBT(current_msg);

}

void SparkDataControl::processPresetEdit(int presetNum) {
	if (presetNum == 0) {
		processDeletePresetRequest();
	} else if (presetEditMode_ == PRESET_EDIT_STORE) {
		processStorePresetRequest(presetNum);
	} else {
		resetPresetEdit(true, true);
		activePresetNum_ = presetNum;
		activePreset_ = presetBuilder.getPreset(activeBank_, activePresetNum_);
		pendingPreset_ = activePreset_;

	}
}

void SparkDataControl::processStorePresetRequest(int presetNum) {
	int responseCode;
	responseMsg_ = "";
	if (presetEditMode_ == PRESET_EDIT_STORE) {
		if (presetNumToEdit_ == presetNum
				&& presetBankToEdit_ == pendingBank_) {
			responseCode = presetBuilder.storePreset(appReceivedPreset_,
					pendingBank_, presetNum);
			if (responseCode == STORE_PRESET_OK) {
				Serial.println("Successfully stored preset");
				resetPresetEdit(true, true);
				appReceivedPreset_ = { };
				activePresetNum_ = presetNum;
				activePreset_ = presetBuilder.getPreset(activeBank_,
						activePresetNum_);
				pendingPreset_ = activePreset_;
				responseMsg_ = "SAVE OK";
			}
			if (responseCode == STORE_PRESET_FILE_EXISTS) {
				responseMsg_ = "PRST EXIST";
			}
			if (responseCode == STORE_PRESET_ERROR_OPEN
					|| responseCode == STORE_PRESET_UNKNOWN_ERROR) {
				responseMsg_ = "SAVE ERROR";
			}
		} else {
			activePresetNum_ = presetNum;
			activePreset_ = presetBuilder.getPreset(activeBank_,
					activePresetNum_);
			pendingPreset_ = activePreset_;
			presetNumToEdit_ = presetNum;
			presetBankToEdit_ = pendingBank_;
		}
	}

}

void SparkDataControl::resetPresetEdit(bool resetEditMode, bool resetPreset) {
	presetNumToEdit_ = 0;
	presetBankToEdit_ = 0;

	if (resetPreset) {
		appReceivedPreset_ = { };
	}
	if (resetEditMode) {
		presetEditMode_ = PRESET_EDIT_NONE;
	}

}

void SparkDataControl::resetPresetEditResponse() {
	responseMsg_ = "";
}

void SparkDataControl::processDeletePresetRequest() {
	int responseCode;
	responseMsg_ = "";
	if (presetEditMode_ == PRESET_EDIT_DELETE && activeBank_ > 0) {
		responseCode = presetBuilder.deletePreset(activeBank_,
				activePresetNum_);
		if (responseCode == DELETE_PRESET_OK
				|| responseCode == DELETE_PRESET_FILE_NOT_EXIST) {
			Serial.printf("Successfully deleted preset %d-%d\n", pendingBank_,
					activePresetNum_);
			presetNumToEdit_ = 0;
			presetBankToEdit_ = 0;
			activePreset_ = presetBuilder.getPreset(pendingBank_,
					activePresetNum_);
			pendingPreset_ = activePreset_;
			if (responseCode == DELETE_PRESET_OK) {
				responseMsg_ = "DELETE OK";
			} else {
				responseMsg_ = "FILE NOT EXITS";
			}
		}
		if (responseCode == DELETE_PRESET_ERROR_OPEN
				|| responseCode == STORE_PRESET_UNKNOWN_ERROR) {
			responseMsg_ = "DELETE ERROR";
		}
		resetPresetEdit(true, true);
	} else {
		setPresetDeletionFlag();
		presetNumToEdit_ = activePresetNum_;
		presetBankToEdit_ = activeBank_;
	}

}

void SparkDataControl::setPresetDeletionFlag() {
	presetEditMode_ = PRESET_EDIT_DELETE;
}

void SparkDataControl::updateActiveWithPendingPreset() {
	activePreset_ = pendingPreset_;
}


void SparkDataControl::sendButtonPressAsKeyboard(keyboardKeyDefinition k) {
	if (bleKeyboard.isConnected()) {

		Serial.printf("Sending button: %d - mod: %d - repeat: %d\n", k.key, k.modifier, k.repeat);
		if (k.modifier != 0) bleKeyboard.press(k.modifier);
		for (uint8_t i=0; i<= k.repeat ; i++) {
			bleKeyboard.write(k.key);
		}
		if (k.modifier != 0) bleKeyboard.release(k.modifier);
		lastKeyboardButtonPressed_ = k.key_uid;
		lastKeyboardButtonPressedString_ = k.display;
	}
	else {
		Serial.println("Keyboard not connected");
	}
}

void SparkDataControl::resetLastKeyboardButtonPressed() {
	lastKeyboardButtonPressed_ = 0;
	lastKeyboardButtonPressedString_ = "";
}

void SparkDataControl::toggleBTMode() {

	if (operationMode_ == SPARK_MODE_AMP){
		Serial.print("Switching Bluetooth mode to ");
		if (currentBTMode_ == BT_MODE_BLE) {
			Serial.println("Serial");
			currentBTMode_ = BT_MODE_SERIAL;
		} else if (currentBTMode_ == BT_MODE_SERIAL) {
			Serial.println("BLE");
			currentBTMode_ = BT_MODE_BLE;
		}
		// Save new mode to file
		fileSystem.saveToFile(btModeFileName.c_str(), currentBTMode_);
		fileSystem.saveToFile(sparkModeFileName.c_str(), sparkModeAmp);
		Serial.println("Restarting in new BT mode");
		restartESP(false);
	}
}

void SparkDataControl::restartESP(bool resetSparkMode){
	//RESET Ignitron
	Serial.print("!!! Restarting !!! ");
	if (resetSparkMode) {
		Serial.print("Resetting Spark mode");
		bool sparkModeFileExists = SPIFFS.exists(sparkModeFileName.c_str());
		if(sparkModeFileExists){
			SPIFFS.remove(sparkModeFileName.c_str());
		}
	}
	Serial.println();
	ESP.restart();
}

void SparkDataControl::setBank(int i){
	if (i > presetBuilder.getNumberOfBanks() || i < 0) return;
	activeBank_ = pendingBank_ = i;

}

void SparkDataControl::increaseBank(){

	if (!processAction()) { return; }

	// Cycle around if at the end
	if (pendingBank_ == numberOfBanks()) {
		// Roll over to 0 when going beyond the last bank
		pendingBank_ = 0;
	} else {
		pendingBank_++;
	}
	if (operationMode_ == SPARK_MODE_AMP && pendingBank_ == 0) {
		pendingBank_ = min(1, numberOfBanks());
	}
	updatePendingBankStatus();
}

void SparkDataControl::decreaseBank(){

	if (!processAction()) { return; }

	// Roll over to last bank if going beyond the first bank
	if (pendingBank_ == 0) {
		pendingBank_ = numberOfBanks();
	} else {
		pendingBank_--;
	}
	// Don't go to bank 0 in AMP mode
	if (operationMode_ == SPARK_MODE_AMP && pendingBank_ == 0) {
		pendingBank_ = numberOfBanks();
	}
	updatePendingBankStatus();

}

void SparkDataControl::updatePendingBankStatus(){
	// Mark selected bank as the pending Bank to mark in display.
	// The device is configured so that it does not immediately
	// switch presets when a bank is changed, it does so only when
	// the preset button is pushed afterwards
	if (pendingBank_ != 0) {
		updatePendingPreset(pendingBank_);
	}
	if (operationMode_ == SPARK_MODE_AMP) {
		activeBank_ = pendingBank_;
		updateActiveWithPendingPreset();
		if (presetEditMode_ == PRESET_EDIT_DELETE) {
			resetPresetEdit(true, true);
		} else if (presetEditMode_ == PRESET_EDIT_STORE) {
			resetPresetEdit(false, false);
		}
	}

}

bool SparkDataControl::toggleEffect(int fx_identifier){
	if (!processAction() || operationMode_ == SPARK_MODE_AMP) {
		Serial.println("Not connected to Spark Amp or in AMP mode, doing nothing.");
		return false;
	}
	if (!activePreset_.isEmpty) {
		string fx_name = activePreset_.pedals[fx_identifier].name;
		bool fx_isOn = activePreset_.pedals[fx_identifier].isOn;

		return switchEffectOnOff(fx_name, fx_isOn ? false : true);
	}
	return false;
}

bool SparkDataControl::getAmpName() {
	current_msg = spark_msg.get_amp_name(nextMessageNum);
	DEBUG_PRINTLN("Getting amp name from Spark");

	return sendMessageToBT(current_msg);

}

bool SparkDataControl::toggleButtonMode(){

	if (!processAction() || operationMode_ == SPARK_MODE_AMP ) {
		Serial.println("Spark Amp not connected or in AMP mode, doing nothing.");
		return false;
	}

	Serial.print("Switching to ");
	switch (buttonMode_) {
	case SWITCH_MODE_FX:
		Serial.println("PRESET mode");
		buttonMode_ = SWITCH_MODE_PRESET;
		break;
	case SWITCH_MODE_PRESET:
		Serial.println("FX mode");
		buttonMode_ = SWITCH_MODE_FX;
		updatePendingWithActive();
		break;
	default:
		Serial.println("Unexpected mode. Defaulting to PRESET mode");
		buttonMode_ = SWITCH_MODE_PRESET;
		break;
	} // SWITCH
	return true;
}

bool SparkDataControl::toggleLooperAppMode(){

	if (operationMode_ == SPARK_MODE_AMP || !processAction()) {
		Serial.println("Spark Amp not connected or in AMP mode, doing nothing.");
		return false;
	}
	int newOperationMode;
	Serial.print("Switching to ");
	switch (operationMode_) {
	case SPARK_MODE_APP:
		Serial.println("LOOPER mode");
		newOperationMode = SPARK_MODE_LOOPER;
		break;
	case SPARK_MODE_LOOPER:
		Serial.println("APP mode");
		newOperationMode = SPARK_MODE_APP;
		break;
	default:
		Serial.println("Unexpected mode. Defaulting to APP mode");
		newOperationMode = SPARK_MODE_APP;
		break;
	} // SWITCH
	switchOperationMode(newOperationMode);
	return true;
}

bool SparkDataControl::handleDeletePreset(){

	if (operationMode_ != SPARK_MODE_AMP) {
		Serial.println("Delete Preset: Not in AMP mode, doing nothing.");
		return false;
	}

	if (presetEditMode() == PRESET_EDIT_STORE) {
		resetPresetEdit(true, true);
	}
	else {
		processPresetEdit();
	}
	return true;
}

bool SparkDataControl::processAction(){

	if (operationMode_ == SPARK_MODE_AMP) {
		return true;
	}
	return isAmpConnected();

}

bool SparkDataControl::processPresetSelect(int presetNum){

	if (!processAction()) {
		Serial.println("Action not meeting requirements, ignoring.");
		return false;
	}

	if (operationMode_ == SPARK_MODE_APP
			|| operationMode_ == SPARK_MODE_LOOPER) {
		switchPreset(presetNum, false);
	} else if (operationMode_ == SPARK_MODE_AMP) { // AMP mode
		processPresetEdit(presetNum);
	}
	return true;
}

bool SparkDataControl::increasePresetLooper(){

	if (!processAction() || operationMode_ != SPARK_MODE_LOOPER) {
		Serial.println("Looper preset change: Spark Amp not connected or not in Looper mode, doing nothing");
		return false;
	}

	int selectedPresetNum;
	if (activePresetNum_ == 4) {
		selectedPresetNum = 1;
		if (activeBank_ == numberOfBanks()) {
			pendingBank_ = 0;
		} else {
			pendingBank_++;
		}
	} else {
		selectedPresetNum = activePresetNum_ + 1;
	}
	return switchPreset(selectedPresetNum, false);
}

bool SparkDataControl::decreasePresetLooper(){

	if (!processAction() || operationMode_ != SPARK_MODE_LOOPER) {
		Serial.println("Looper preset change: Spark Amp not connected or not in Looper mode, doing nothing");
		return false;
	}

	int selectedPresetNum;
	if (activePresetNum_== 1) {
		selectedPresetNum = 4;
		if (activeBank_ == 0) {
			pendingBank_ = numberOfBanks();
		} else {
			pendingBank_--;
		}
	} else {
		selectedPresetNum = activePresetNum_ - 1;
	}
	return switchPreset(selectedPresetNum, false);
}

bool SparkDataControl::sendMessageToBT(vector<ByteVector> msg){
	nextMessageNum++;
	//spark_ssr.clearMessageBuffer();
	DEBUG_PRINTLN("Sending message via BT.");
	return bleControl->writeBLE(msg, with_delay);
}

void SparkDataControl::handleSendingAck(ByteVector blk) {
	bool ackNeeded;
	byte seq, sub_cmd;

	// Check if ack needed. In positive case the sequence number and command
	// are also returned to send back to requester
	tie(ackNeeded, seq, sub_cmd) = spark_ssr.needsAck(blk);
	if (ackNeeded) {
		if (operationMode_ == SPARK_MODE_AMP) {
			ack_msg = spark_msg.send_ack(seq, sub_cmd, DIR_FROM_SPARK);
		} else {
			ack_msg = spark_msg.send_ack(seq, sub_cmd, DIR_TO_SPARK);
		}

		DEBUG_PRINTLN("Sending acknowledgment");
		if (operationMode_ == SPARK_MODE_APP || operationMode_ == SPARK_MODE_LOOPER) {
			sendMessageToBT(ack_msg);
		} else if (operationMode_ == SPARK_MODE_AMP) {
			bleControl->notifyClients(ack_msg);
		}
	}

}

void SparkDataControl::handleAmpModeRequest() {

	vector<ByteVector> msg;
	vector<CmdData> currentMessage = spark_ssr.lastMessage();
	byte currentMessageNum = spark_ssr.lastMessageNum();
	byte sub_cmd_ = currentMessage.back().subcmd;

	switch (sub_cmd_) {

	case 0x23:
		DEBUG_PRINTLN("Found request for serial number");
		msg = spark_msg.send_serial_number(
				currentMessageNum);
		break;
	case 0x2F:
		DEBUG_PRINTLN("Found request for firmware version");
		msg = spark_msg.send_firmware_version(
				currentMessageNum);
		break;
	case 0x2A:
		DEBUG_PRINTLN("Found request for hw checksum");
		msg = spark_msg.send_hw_checksums(currentMessageNum);
		break;
	case 0x10:
		DEBUG_PRINTLN("Found request for hw preset number");
		msg = spark_msg.send_hw_preset_number(currentMessageNum);
		break;
	case 0x01:
		DEBUG_PRINTLN("Found request for current preset");
		msg = spark_msg.create_preset(activePreset_, DIR_FROM_SPARK,
				currentMessageNum);
		break;
	case 0x71:
		DEBUG_PRINTLN("Found request for 02 71");
		msg = spark_msg.send_response_71(currentMessageNum);
		break;
	case 0x72:
		DEBUG_PRINTLN("Found request for 02 72");
		msg = spark_msg.send_response_72(currentMessageNum);
		break;
	default:
		DEBUG_PRINTF("Found invalid request: %02x \n", sub_cmd_);
		break;
	}

	bleControl->notifyClients(msg);

}

void SparkDataControl::handleAppModeResponse() {

	string msgStr = spark_ssr.getJson();
	int lastMessageType = spark_ssr.lastMessageType();
	bool printMessage = false;
	if (operationMode_ == SPARK_MODE_APP) {

		if (lastMessageType == MSG_TYPE_HWPRESET) {
			DEBUG_PRINTLN("Received HW Preset response");
			int spark_presetNumber = spark_ssr.currentPresetNumber();

			// only change active presetNumber if new number is between 1 and 4,
			// otherwise it is a custom preset number and can be ignored
			if(spark_presetNumber >= 1 && spark_presetNumber <= 4){
				activeBank_ = pendingBank_ = 0;
				activePresetNum_= spark_presetNumber;
			}
			else {
				DEBUG_PRINTLN("Received custom preset number (128), ignoring number change");
			}
			printMessage = true;
		}

		if (lastMessageType == MSG_TYPE_PRESET) {
			DEBUG_PRINTLN("Last message was a preset change.");
			printMessage = true;
		}

		if (lastMessageType == MSG_TYPE_AMP_NAME) {
			DEBUG_PRINTLN("Last message was amp name.");
			sparkAmpName = spark_ssr.getAmpName();
			setAmpParameters();
			printMessage = true;
		}
		if (msgStr.length() > 0 && printMessage) {
			Serial.println("Message processed:");
			Serial.println(msgStr.c_str());
		}
	}

	if (operationMode_ == SPARK_MODE_AMP) {
		if (lastMessageType == MSG_TYPE_PRESET) {
			presetEditMode_ = PRESET_EDIT_STORE;
			appReceivedPreset_ = presetBuilder.getPresetFromJson(&msgStr[0]);
			DEBUG_PRINTLN("received from app:");
			DEBUG_PRINTLN(appReceivedPreset_.json.c_str());
			spark_ssr.resetPresetUpdateFlag();
			spark_ssr.resetPresetNumberUpdateFlag();
			presetNumToEdit_ = 0;
		}
	}
	spark_ssr.resetLastMessageType();
}

void SparkDataControl::handleIncomingAck() {

	// if last Ack was for preset change (0x01 / 0x38) or effect switch (0x15),
	// confirm pending preset into active
	AckData lastAck = spark_ssr.getLastAckAndEmpty();
	if (lastAck.cmd == 0x04){
		DEBUG_PRINTLN("Received ACK");
		if (lastAck.subcmd == 0x01) {
			// only execute preset number change on first ack for preset change
			if (customPresetNumberChangePending) {
				current_msg = spark_msg.change_hardware_preset(nextMessageNum, 128);
				sendMessageToBT(current_msg);
				customPresetNumberChangePending = false;
				activePreset_ = pendingPreset_;
				Serial.println("OK");
			}
		}
		if (lastAck.subcmd == 0x38) {
			DEBUG_PRINTLN("Received ACK for 0x38 command, getting preset from Spark.");
			getCurrentPresetFromSpark();
			activePreset_ = pendingPreset_;
			Serial.println("OK");
		}
		if (lastAck.subcmd == 0x15) {
			activePreset_ = pendingPreset_;
			Serial.println("OK");
		}
	}
}


void SparkDataControl::setAmpParameters() {

	string ampName = sparkAmpName;
	DEBUG_PRINTF("Amp name: %s\n", ampName.c_str());
	if (ampName == AMP_NAME_SPARK_40){
		spark_msg.maxChunkSizeToSpark() = 0x80;
		spark_msg.maxBlockSizeToSpark() = 0xAD;
		spark_msg.withHeader() = true;
		with_delay = false;
	}
	if (ampName == AMP_NAME_SPARK_MINI || ampName == AMP_NAME_SPARK_GO) {
		spark_msg.maxChunkSizeToSpark() = 0x27;
		spark_msg.maxBlockSizeToSpark() = 0x14;
		spark_msg.withHeader() = false;
		with_delay = true;
	}
	spark_msg.maxChunkSizeFromSpark() = 0x19;
	spark_msg.maxBlockSizeFromSpark() = 0x6A;


}
