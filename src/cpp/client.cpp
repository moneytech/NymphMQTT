/*
	client.cpp - Implementation for the NymphMQTT Client class.
	
	Revision 0
	
	Features:
			- 
			
	Notes:
			- 
			
	2019/05/08 - Maya Posch
*/


#include "client.h"
#include "client_listener_manager.h"
#include "connections.h"
#include "dispatcher.h"

#include <Poco/Net/NetException.h>
#include <Poco/NumberFormatter.h>

using namespace Poco;

using namespace std;


// --- CONSTRUCTOR ---
NmqttClient::NmqttClient() {
	//
}


// --- INIT ---
// Initialise the runtime and sets the logger function to be used by the Nymph 
// Logger class, along with the desired maximum log level:
// NYMPH_LOG_LEVEL_FATAL = 1,
// NYMPH_LOG_LEVEL_CRITICAL,
// NYMPH_LOG_LEVEL_ERROR,
// NYMPH_LOG_LEVEL_WARNING,
// NYMPH_LOG_LEVEL_NOTICE,
// NYMPH_LOG_LEVEL_INFO,
// NYMPH_LOG_LEVEL_DEBUG,
// NYMPH_LOG_LEVEL_TRACE
bool NmqttClient::init(std::function<void(int, std::string)> logger, int level, long timeout) {	
	//NymphRemoteServer::timeout = timeout; // FIXME
	setLogger(logger, level);
	
	// Get the number of concurrent threads supported by the system we are running on.
	int numThreads = std::thread::hardware_concurrency();
	
	// Initialise the Dispatcher with the maximum number of threads as worker count.
	Dispatcher::init(numThreads);
	
	return true;
}


// --- SET LOGGER ---
// Sets the logger function to be used by the Nymph Logger class, along with the
// desired maximum log level:
// NYMPH_LOG_LEVEL_FATAL = 1,
// NYMPH_LOG_LEVEL_CRITICAL,
// NYMPH_LOG_LEVEL_ERROR,
// NYMPH_LOG_LEVEL_WARNING,
// NYMPH_LOG_LEVEL_NOTICE,
// NYMPH_LOG_LEVEL_INFO,
// NYMPH_LOG_LEVEL_DEBUG,
// NYMPH_LOG_LEVEL_TRACE
void NmqttClient::setLogger(std::function<void(int, std::string)> logger, int level) {
	NymphLogger::setLoggerFunction(logger);
	NymphLogger::setLogLevel((Poco::Message::Priority) level);
}


// --- SET MESSAGE HANDLER ---
// Set the callback function that will be called every time a message is received from the broker.
void NmqttClient::setMessageHandler(std::function<void(int, std::string, std::string)> handler) {
	messageHandler = handler;
}


// --- SHUTDOWN ---
// Shutdown the runtime. Close any open connections and clean up resources.
bool NmqttClient::shutdown() {
	socketsMutex.lock();
	map<int, Poco::Net::StreamSocket*>::iterator it;
	for (it = sockets.begin(); it != sockets.end(); ++it) {
		// Remove socket from listener.
		NmqttClientListenerManager::removeConnection(it->first);
		
		// TODO: try/catch. 
		it->second->shutdown();
	}
	
	sockets.clear();
	socketsMutex.unlock();
	
	NmqttClientListenerManager::stop();
	
	return true;
}

 
// --- CONNECT ---
// Create a new connection with the remote MQTT server and return a handle for
// the connection.
bool NmqttClient::connect(string host, int port, int &handle, void* data, 
							NmqttBrokerConnection &conn, string &result) {
	Poco::Net::SocketAddress sa(host, port);
	return connect(sa, handle, data, conn, result);
}


bool NmqttClient::connect(string url, int &handle, void* data, 
							NmqttBrokerConnection &conn, string &result) {
	Poco::Net::SocketAddress sa(url);
	return connect(sa, handle, data, conn, result);
}


bool NmqttClient::connect(Poco::Net::SocketAddress sa, int &handle,  void* data,
							NmqttBrokerConnection &conn, string &result) {
	using namespace std::placeholders;
	Poco::Net::StreamSocket* socket;
	try {
		socket = new Poco::Net::StreamSocket(sa);
	}
	catch (Poco::Net::ConnectionRefusedException &ex) {
		// Handle connection error.
		result = "Unable to connect: " + ex.displayText();
		return false;
	}
	catch (Poco::InvalidArgumentException &ex) {
		result = "Invalid argument: " + ex.displayText();
		return false;
	}
	catch (Poco::Net::InvalidSocketException &ex) {
		result = "Invalid socket exception: " + ex.displayText();
		return false;
	}
	catch (Poco::Net::NetException &ex) {
		result = "Net exception: " + ex.displayText();
		return false;
	}
	
	socketsMutex.lock();
	sockets.insert(pair<int, Poco::Net::StreamSocket*>(lastHandle, socket));
	NymphSocket ns;
	ns.socket = socket;
	ns.semaphore = new Semaphore(0, 1);
	socketSemaphores.insert(pair<int, Poco::Semaphore*>(lastHandle, ns.semaphore));
	ns.data = data;
	ns.handle = lastHandle;
	ns.handler = messageHandler;
	ns.connackHandler = std::bind(&NmqttClient::connackHandler, this, _1, _2, _3);
	ns.pingrespHandler = std::bind(&NmqttClient::pingrespHandler, this, _1);
	NmqttConnections::addSocket(ns);
	NmqttClientListenerManager::addConnection(lastHandle);
	handle = lastHandle++;
	socketsMutex.unlock();
	
	NYMPH_LOG_DEBUG("Added new connection with handle: " + NumberFormatter::format(handle));
	
	// Send Connect message using the previously set data.
	brokerConn = 0;
	NmqttMessage msg(MQTT_CONNECT);
	msg.setWill(will);
	msg.setClientId(clientId);
	
	NYMPH_LOG_INFORMATION("Sending CONNECT message.");
	
	if (!sendMessage(handle, msg.serialize())) {
		return false;
	}
	
	// Wait for condition.
	connectMtx.lock();
	brokerConn = &conn;
	long timeout = 3000; // 3 second connection timeout.
	if (!connectCnd.tryWait(connectMtx, timeout)) {
		NYMPH_LOG_ERROR("Timeout while trying to connect to broker.");
		brokerConn = 0;
		connectMtx.unlock();
		return false;
	}
	
	// Start ping timer. Use the Keep Alive value used during the Connect minus one second as 
	// duration.
	// FIXME: check that the Keep Alive value isn't less than one second. Subtract milliseconds if 
	// less than 10 seconds or so.
	int keepAlive = 60; // In seconds. TODO: use connection Keep Alive value.
	pingTimer = new NmqttTimer((keepAlive - 2) * 1000, (keepAlive - 2) * 1000);
	pingCallback = new Poco::TimerCallback<NmqttClient>(*this, &NmqttClient::pingreqHandler);
	
	try {
		pingTimer->stop();
		pingTimer->setHandle(handle);
		pingTimer->start(*pingCallback);
		NYMPH_LOG_INFORMATION("Started ping timer...");
	}
	catch (Poco::IllegalStateException &e) {
		NYMPH_LOG_ERROR("IllegalStateException on ping timer start: " + e.message());
		return false;
	}
	catch (...) {
		NYMPH_LOG_ERROR("Unknown exception on ping timer start.");
		return false;
	}
	
	
	return true;
}


// --- DISCONNECT ---
bool NmqttClient::disconnect(int handle, string &result) {
	// Stop the Pingreq timer and delete it.
	delete pingTimer;
	delete pingCallback;
	
	// Create a Disconnect message, send it to the indicated remote.
	NYMPH_LOG_INFORMATION("Sending DISCONNECT message.");
	NmqttMessage msg(MQTT_DISCONNECT);
	msg.setWill(will);
	
	sendMessage(handle, msg.serialize());
	
	// FIXME: wait here?
	
	map<int, Poco::Net::StreamSocket*>::iterator it;
	map<int, Poco::Semaphore*>::iterator sit;
	socketsMutex.lock();
	it = sockets.find(handle);
	if (it == sockets.end()) { 
		result = "Provided handle " + NumberFormatter::format(handle) + " was not found.";
		socketsMutex.unlock();
		return false; 
	}
	
	sit = socketSemaphores.find(handle);
	if (sit == socketSemaphores.end()) {
		result = "No semaphore found for socket handle.";
		socketsMutex.unlock();
		return false;
	}
	
	// TODO: try/catch.
	// Shutdown socket. Set the semaphore once done to signal that the socket's 
	// listener thread that it's safe to delete the socket.
	it->second->shutdown();
	it->second->close();
	if (sit->second) { sit->second->set(); }
	
	// Remove socket from listener.
	NmqttClientListenerManager::removeConnection(it->first);
	
	// Remove socket references from both maps.
	sockets.erase(it);
	socketSemaphores.erase(sit);
	
	socketsMutex.unlock();
	
	NYMPH_LOG_DEBUG("Removed connection with handle: " + NumberFormatter::format(handle));
	
	return true;
}


// --- SET WILL ---
// Set the will message for a Connect message.
void NmqttClient::setWill(std::string will) {
	this->will = will;
}


// --- SEND MESSAGE ---
// Private method for sending data to a remote broker.
bool NmqttClient::sendMessage(int handle, std::string binMsg) {
	map<int, Poco::Net::StreamSocket*>::iterator it;
	socketsMutex.lock();
	it = sockets.find(handle);
	if (it == sockets.end()) { 
		NYMPH_LOG_ERROR("Provided handle " + NumberFormatter::format(handle) + " was not found.");
		socketsMutex.unlock();
		return false;
	}
	
	try {
		int ret = it->second->sendBytes(((const void*) binMsg.c_str()), binMsg.length());
		if (ret != binMsg.length()) {
			// Handle error.
			NYMPH_LOG_ERROR("Failed to send message. Not all bytes sent.");
			return false;
		}
		
		NYMPH_LOG_DEBUG("Sent " + NumberFormatter::format(ret) + " bytes.");
	}
	catch (Poco::Exception &e) {
		NYMPH_LOG_ERROR("Failed to send message: " + e.message());
		return false;
	}
	
	socketsMutex.unlock();
	
	// Reset Ping timer.
	if (pingTimer) {
		try {
			pingTimer->stop();
			pingTimer->start(*pingCallback);
			NYMPH_LOG_INFORMATION("Started ping timer...");
		}
		catch (Poco::IllegalStateException &e) {
			NYMPH_LOG_ERROR("IllegalStateException on ping timer start: " + e.message());
			return false;
		}
		catch (...) {
			NYMPH_LOG_ERROR("Unknown exception on ping timer start.");
			return false;
		}
	}
	
	return true;
}


// --- CONNACK HANDLER ---
// Callback for incoming CONNACK packets.
void NmqttClient::connackHandler(int handle, bool sessionPresent, MqttReasonCodes code) {
	// Set the data
	if (brokerConn) {
		brokerConn->handle = handle;
		brokerConn->sessionPresent = sessionPresent;
		brokerConn->responseCode = code;
	}
	
	// Signal the condition variable.
	connectCnd.signal();
}


// --- PINGREQ HANDLER ---
// Callback for the internal timer to send a ping request to the broker to keep the connection
// alive.
void NmqttClient::pingreqHandler(Poco::Timer &t) {
	//
	NmqttMessage msg(MQTT_PINGREQ);
	
	NmqttTimer* s = dynamic_cast<NmqttTimer*>(&t);
	
	NYMPH_LOG_INFORMATION("Sending PINGREQ message for handle: " + 
							Poco::NumberFormatter::format(s->getHandle()));
	
	if (!sendMessage(s->getHandle(), msg.serialize())) {
		NYMPH_LOG_ERROR("Failed to send PINGREQ message.");
	}
}


// --- PINGRESP HANDLER ---
// Called when a PINGACK message arrives. Reset the ping timer for this handle.
// TODO: implement per handle timer.
void NmqttClient::pingrespHandler(int handle) {
	NYMPH_LOG_DEBUG("PINGRESP handler got called.");
	
	long keepAlive = 58 * 1000; // TODO: use global value.
	
	// Reset Ping timer.
	if (pingTimer) {
		NYMPH_LOG_DEBUG("Trying to restart the timer...");
		try {
			NYMPH_LOG_DEBUG("Stopping the timer...");
			//delete pingTimer;
			pingTimer = new NmqttTimer(keepAlive, 0);
			pingTimer->setHandle(handle);
			//pingTimer->restart(0);
			NYMPH_LOG_DEBUG("Stopped the timer.");
			NYMPH_LOG_DEBUG("Starting the timer...");
			//pingTimer->setStartInterval(keepAlive);
			pingTimer->start(*pingCallback);
			NYMPH_LOG_INFORMATION("Started ping timer.");
		}
		catch (Poco::IllegalStateException &e) {
			NYMPH_LOG_ERROR("IllegalStateException on ping timer start: " + e.message());
			return;
		}
		catch (...) {
			NYMPH_LOG_ERROR("Unknown exception on ping timer start.");
			return;
		}
		
		NYMPH_LOG_DEBUG("Successfully restarted the timer.");
	}
}


// --- PUBLISH ---
bool NmqttClient::publish(int handle, std::string topic, std::string payload, std::string &result, 
							MqttQoS qos, bool retain) {
	//
	NmqttMessage msg(MQTT_PUBLISH);
	msg.setQoS(qos);
	msg.setRetain(retain);
	msg.setTopic(topic);
	msg.setPayload(payload);
	
	NYMPH_LOG_INFORMATION("Sending PUBLISH message.");
	
	return sendMessage(handle, msg.serialize());
}


// --- SUBSCRIBE ---
bool NmqttClient::subscribe(int handle, std::string topic, std::string result) {
	//
	NmqttMessage msg(MQTT_SUBSCRIBE);
	msg.setTopic(topic);
	
	NYMPH_LOG_INFORMATION("Sending SUBSCRIBE message.");
	
	return sendMessage(handle, msg.serialize());
}


// --- UNSUBSCRIBE ---
bool NmqttClient::unsubscribe(int handle, std::string topic, std::string result) {
	NmqttMessage msg(MQTT_UNSUBSCRIBE);
	msg.setTopic(topic);
	
	NYMPH_LOG_INFORMATION("Sending UNSUBSCRIBE message.");
	
	return sendMessage(handle, msg.serialize());
}
