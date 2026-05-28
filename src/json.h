#include <FS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

class Json {
    public: 
        bool static loadFile(const char* filePath, JsonDocument& doc);
    private:
};