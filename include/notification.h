#ifndef NOTIFICATION_H
#define NOTIFICATION_H

#include <Arduino.h>

// Ringtone types
enum RingtoneType {
    RINGTONE_NONE = 0,
    RINGTONE_BEEP = 1,     // Simple beep sound
    RINGTONE_BELL = 2,     // Bell-like tone
    RINGTONE_CHIME = 3     // Gentle chime
};

// Notification settings structure
struct NotificationSettings {
    bool broadcastEnabled = true;      // Enable notification for broadcast messages
    bool directMessageEnabled = true;  // Enable notification for direct messages
    RingtoneType broadcastRingtone = RINGTONE_BEEP;   // Ringtone for broadcast
    RingtoneType directMessageRingtone = RINGTONE_BELL; // Ringtone for DM
    uint8_t volume = 50;               // Volume 0-100
};

class NotificationManager {
public:
    NotificationManager();
    ~NotificationManager();
    
    void begin();
    void playRingtone(RingtoneType type);
    void playNotification(bool isBroadcast);
    void stopRingtone();
    
    // Settings management
    void loadSettings();
    void saveSettings();
    NotificationSettings& getSettings() { return settings; }
    void setSettings(const NotificationSettings& newSettings);
    
    // Get ringtone name for display
    static String getRingtoneName(RingtoneType type);
    
private:
    NotificationSettings settings;
    bool speakerAvailable = false;  // True on CardPuter ADV
    
    // Audio generation using M5Cardputer.Speaker API (for CardPuter ADV)
    void playBeep();
    void playBell();
    void playChime();
};

extern NotificationManager* g_notificationManager;

#endif // NOTIFICATION_H
