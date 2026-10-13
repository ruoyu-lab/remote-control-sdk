#include "broadcast_settings.hpp"

#import <Foundation/Foundation.h>
#import <Security/Security.h>

namespace {

template <class T>
NSArray* numbers(const std::vector<T>& values) {
    NSMutableArray* array = [NSMutableArray array];
    for (const T value : values) {
        [array addObject:@(value)];
    }
    return array;
}

template <class T>
std::vector<T> values(id array) {
    std::vector<T> result;
    if ([array isKindOfClass:NSArray.class]) {
        for (NSNumber* number in static_cast<NSArray*>(array)) {
            result.push_back(T(number.unsignedIntValue));
        }
    }
    return result;
}

// Without an access group the item goes to the first one the entitlements
// list, the shared one, in the app and the extension alike.
NSDictionary* itemQuery() {
    return @{
        (id)kSecClass : (id)kSecClassGenericPassword,
        (id)kSecAttrService : @"local.rcsdk.qmldemo.broadcast",
        (id)kSecAttrAccount : @"settings",
    };
}

} // namespace

bool saveBroadcastSettings(const BroadcastSettings& s) {
    NSDictionary* plist = @{
        @"port" : @(s.port),
        @"key" : [NSString stringWithUTF8String:s.key.c_str()],
        @"tcp" : @(s.tcp),
        @"longSide" : @(s.longSide),
        @"framesPerSecond" : @(s.framesPerSecond),
        @"bitrateMbps" : @(s.bitrateMbps),
        @"codecs" : numbers(s.codecs),
        @"audioCodecs" : numbers(s.audioCodecs),
        @"sound" : @(s.sound),
    };
    NSData* data = [NSPropertyListSerialization dataWithPropertyList:plist
                                                              format:NSPropertyListBinaryFormat_v1_0
                                                             options:0
                                                               error:nil];
    if (!data) {
        return false;
    }
    SecItemDelete((__bridge CFDictionaryRef)itemQuery());
    NSMutableDictionary* item = [itemQuery() mutableCopy];
    item[(id)kSecValueData] = data;
    // Readable by the extension while the device is locked after the first unlock.
    item[(id)kSecAttrAccessible] = (id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly;
    if (SecItemAdd((__bridge CFDictionaryRef)item, nullptr) != errSecSuccess) {
        return false;
    }
    CFNotificationCenterPostNotification(CFNotificationCenterGetDarwinNotifyCenter(),
                                         (__bridge CFStringRef)@(broadcastSettingsChanged), nullptr, nullptr, true);
    return true;
}

std::optional<BroadcastSettings> loadBroadcastSettings() {
    NSMutableDictionary* query = [itemQuery() mutableCopy];
    query[(id)kSecReturnData] = @YES;
    query[(id)kSecMatchLimit] = (id)kSecMatchLimitOne;
    CFTypeRef found = nullptr;
    if (SecItemCopyMatching((__bridge CFDictionaryRef)query, &found) != errSecSuccess || !found) {
        return std::nullopt;
    }
    NSData* data = CFBridgingRelease(found);
    NSDictionary* plist = [NSPropertyListSerialization propertyListWithData:data options:0 format:nil error:nil];
    if (![plist isKindOfClass:NSDictionary.class] || ![plist[@"key"] isKindOfClass:NSString.class]) {
        return std::nullopt;
    }
    BroadcastSettings s;
    s.port = uint16_t([plist[@"port"] unsignedIntValue]);
    s.key = [plist[@"key"] UTF8String];
    s.tcp = [plist[@"tcp"] boolValue];
    s.longSide = [plist[@"longSide"] unsignedIntValue];
    s.framesPerSecond = [plist[@"framesPerSecond"] unsignedIntValue];
    s.bitrateMbps = [plist[@"bitrateMbps"] unsignedIntValue];
    if (auto codecs = values<rcs_codec>(plist[@"codecs"]); !codecs.empty()) {
        s.codecs = std::move(codecs);
    }
    if (auto codecs = values<rcs_audio_codec>(plist[@"audioCodecs"]); !codecs.empty()) {
        s.audioCodecs = std::move(codecs);
    }
    s.sound = [plist[@"sound"] boolValue];
    return s;
}
