#include "db.h"
#include <iostream>
#include <sstream>
#include <vector>
#include <thread>
#include <atomic>

// Thread function for concurrent writing
void writer_thread(ZenithDB& db, int id, int count) {
    for (int i = 0; i < count; ++i) {
        std::string key = "thread:" + std::to_string(id) + ":k" + std::to_string(i);
        std::string val = "value_from_thread_" + std::to_string(id);
        db.put(key, val);
    }
}

int main() {
    // Updated constructor: dir="mydb", node_id="node1", sync=false
    ZenithDB db("mydb", "node1", false); 

    std::cout << "ZenithDB Console (Node: node1)\n";
    std::cout << "Commands: put <key> <value> | get <key> | del <key> | scan [<start> [<end>]] | stress | exit\n";

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
                    std::cout << k << " -> " << v << '\n';
                }
            }
        }
        else if (cmd == "stress") {
            int threads = 4;
            int ops_per_thread = 5000;
            std::cout << "Starting stress test with " << threads << " threads, " << ops_per_thread << " ops each...\n";
            
            std::vector<std::thread> pool;
            for(int i=0; i<threads; ++i) {
                pool.emplace_back(writer_thread, std::ref(db), i, ops_per_thread);
            }
            
            for(auto& t : pool) t.join();
            std::cout << "Stress test complete.\n";
        }
        else {
            std::cout << "Unknown command.\n";
        }
    }
    std::cout << "Bye!\n";
    return 0;
}