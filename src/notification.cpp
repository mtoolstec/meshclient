#include "notification.h"
#include <Preferences.h>
#include <cmath>

#ifdef CARDPUTER_ADV
#include <M5Cardputer.h>
#endif

NotificationManager* g_notificationManager = nullptr;

NotificationManager::NotificationManager() {
}

NotificationManager::~NotificationManager() {
    // Nothing to clean up when using M5Cardputer.Speaker
}

void NotificationManager::begin() {
    loadSettings();
    
#ifdef CARDPUTER_ADV
    // CardPuter ADV has built-in speaker, use M5Cardputer.Speaker API
    Serial.println("[Notification] CardPuter ADV - Using M5Cardputer.Speaker");
    speakerAvailable = true;
#else
    // Standard CardPuter doesn't have speaker hardware
    Serial.println("[Notification] Standard CardPuter - No speaker hardware");
    speakerAvailable = false;
#endif
    
    Serial.println("[Notification] Manager initialized");
}

// Audio playback functions using M5Cardputer.Speaker API

void NotificationManager::playRingtone(RingtoneType type) {
    Serial.printf("[Notification] playRingtone called: type=%d, speakerAvailable=%d, volume=%d\n", 
                  (int)type, speakerAvailable, settings.volume);
    
    if (!speakerAvailable) {
        Serial.println("[Notification] ✗ Cannot play - Speaker not available (standard CardPuter)");
        return;
    }
    
    if (type == RINGTONE_NONE || settings.volume == 0) {
        Serial.println("[Notification] ✗ Cannot play - type is NONE or volume is 0");
        return;
    }
    
    Serial.printf("[Notification] ▶ Playing ringtone type %d at volume %d%%\n", (int)type, settings.volume);
    
    switch (type) {
        case RINGTONE_BEEP:
            playBeep();
            break;
        case RINGTONE_BELL:
            playBell();
            break;
        case RINGTONE_CHIME:
            playChime();
            break;
        default:
            Serial.println("[Notification] ✗ Unknown ringtone type");
            return;
    }
    
    Serial.printf("[Notification] ✓ Finished playing ringtone type %d\n", (int)type);
}

void NotificationManager::playBeep() {
#ifdef CARDPUTER_ADV
    Serial.println("[Notification] Playing BEEP using M5Cardputer.Speaker");
    // Simple beep: 1000Hz for 200ms
    M5Cardputer.Speaker.tone(1000, 200);
#else
    Serial.println("[Notification] Cannot play BEEP - no speaker on standard CardPuter");
#endif
}

void NotificationManager::playBell() {
#ifdef CARDPUTER_ADV
    Serial.println("[Notification] Playing BELL using M5Cardputer.Speaker");
    // Bell-like tone: sequence of frequencies
    M5Cardputer.Speaker.tone(800, 100);
    delay(50);
    M5Cardputer.Speaker.tone(1000, 100);
    delay(50);
    M5Cardputer.Speaker.tone(1200, 150);
#else
    Serial.println("[Notification] Cannot play BELL - no speaker on standard CardPuter");
#endif
}

void NotificationManager::playChime() {
#ifdef CARDPUTER_ADV
    Serial.println("[Notification] Playing CHIME using M5Cardputer.Speaker");
    // Gentle chime: ascending tones
    M5Cardputer.Speaker.tone(523, 100);  // C5
    delay(50);
    M5Cardputer.Speaker.tone(659, 100);  // E5
    delay(50);
    M5Cardputer.Speaker.tone(784, 150);  // G5
#else
    Serial.println("[Notification] Cannot play CHIME - no speaker on standard CardPuter");
#endif
}

void NotificationManager::playNotification(bool isBroadcast) {
    if (!speakerAvailable) {
        return;
    }
    
    if (isBroadcast) {
        if (!settings.broadcastEnabled) return;
        playRingtone(settings.broadcastRingtone);
    } else {
        if (!settings.directMessageEnabled) return;
        playRingtone(settings.directMessageRingtone);
    }
}

void NotificationManager::stopRingtone() {
#ifdef CARDPUTER_ADV
    M5Cardputer.Speaker.stop();
#endif
}

void NotificationManager::loadSettings() {
    Preferences prefs;
    if (prefs.begin("notification", true)) {
        settings.broadcastEnabled = prefs.getBool("bc_enabled", true);
        settings.directMessageEnabled = prefs.getBool("dm_enabled", true);
        settings.broadcastRingtone = (RingtoneType)prefs.getUChar("bc_ringtone", RINGTONE_BEEP);
        settings.directMessageRingtone = (RingtoneType)prefs.getUChar("dm_ringtone", RINGTONE_BELL);
        settings.volume = prefs.getUChar("volume", 50);
        prefs.end();
        Serial.println("[Notification] Settings loaded");
    }
}

void NotificationManager::saveSettings() {
    Preferences prefs;
    if (prefs.begin("notification", false)) {
        prefs.putBool("bc_enabled", settings.broadcastEnabled);
        prefs.putBool("dm_enabled", settings.directMessageEnabled);
        prefs.putUChar("bc_ringtone", (uint8_t)settings.broadcastRingtone);
        prefs.putUChar("dm_ringtone", (uint8_t)settings.directMessageRingtone);
        prefs.putUChar("volume", settings.volume);
        prefs.end();
        Serial.println("[Notification] Settings saved");
    }
}

void NotificationManager::setSettings(const NotificationSettings& newSettings) {
    settings = newSettings;
    saveSettings();
}

String NotificationManager::getRingtoneName(RingtoneType type) {
    switch (type) {
        case RINGTONE_NONE: return "None";
        case RINGTONE_BEEP: return "Beep";
        case RINGTONE_BELL: return "Bell";
        case RINGTONE_CHIME: return "Chime";
        default: return "Unknown";
    }
}
