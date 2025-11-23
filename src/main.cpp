// src/main.cpp
#include "db.h"
#include <iostream>
#include <sstream>
#include <vector>

int main() {
    ZenithDB db("mydb");        // folder will be created automatically

    std::string line;
    while (std::cout << "> " && std::getline(std::cin, line)) {
        if (line.empty()) continue;
        if (line == "exit" || line == "quit") break;

        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        if (cmd == "put") {
            std::string key, value;
            iss >> key;
            std::getline(iss, value);
            if (!value.empty()) value.erase(0, value.find_first_not_of(" \t"));
            if (key.empty()) {
                std::cout << "Usage: put <key> <value>\n";
                continue;
            }
            db.put(key, value);
            std::cout << "OK\n";
        }
        else if (cmd == "get") {
            std::string key;
            iss >> key;
            if (key.empty()) {
                std::cout << "Usage: get <key>\n";
                continue;
            }
            auto val = db.get(key);
            if (val) std::cout << *val << '\n';
            else std::cout << "(nil)\n";
        }
        else if (cmd == "del") {
            std::string key;
            iss >> key;
            if (key.empty()) {
                std::cout << "Usage: del <key>\n";
                continue;
            }
            db.remove(key);
            std::cout << "OK\n";
        }
        else if (cmd == "scan") {
            std::string start, end;
            iss >> start >> end;

            // default: scan everything
            if (start.empty()) start = "";
            if (end.empty())   end   = "\xFF\xFF\xFF\xFF";

            auto results = db.scan(start, end);
            if (results.empty()) {
                std::cout << "(empty)\n";
            } else {
                for (const auto& [k, v] : results) {
                    std::cout << k << " → " << v << '\n';
                }
            }
        }
        else {
            std::cout << "Commands: put <key> <value> | get <key> | del <key> | scan [<start> [<end>]] | exit\n";
        }
    }
    std::cout << "Bye!\n";
    return 0;
}