#include "hermes/tools/todo_tool.hpp"

#include "hermes/tools/registry.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace hermes::tools {

namespace {

struct TodoItem {
    std::string id;
    std::string content;
    std::string status;  // pending | in_progress | completed | cancelled
};

bool valid_status(const std::string& s) {
    return s == "pending" || s == "in_progress" || s == "completed" ||
           s == "cancelled";
}

std::mutex& todos_mu() {
    static std::mutex mu;
    return mu;
}

std::map<std::string, std::vector<TodoItem>>& todos_store() {
    static std::map<std::string, std::vector<TodoItem>> store;
    return store;
}

int& next_id_counter() {
    static int counter = 0;
    return counter;
}

std::string generate_id() {
    return "todo_" + std::to_string(++next_id_counter());
}

nlohmann::json items_to_json(const std::vector<TodoItem>& items) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& item : items) {
        arr.push_back(
            {{"id", item.id}, {"content", item.content}, {"status", item.status}});
    }
    return arr;
}

std::vector<TodoItem> parse_todo_items(const nlohmann::json& arr) {
    std::vector<TodoItem> items;
    for (const auto& obj : arr) {
        TodoItem item;
        item.id = obj.value("id", "");
        item.content = obj.value("content", "");
        item.status = obj.value("status", "pending");
        if (item.id.empty()) {
            item.id = generate_id();
        }
        items.push_back(std::move(item));
    }
    return items;
}

}  // namespace

void clear_all_todos() {
    std::lock_guard<std::mutex> lk(todos_mu());
    todos_store().clear();
    next_id_counter() = 0;
}

void register_todo_tools(ToolRegistry& registry) {
    ToolEntry e;
    e.name = "todo";
    e.toolset = "memory";
    e.description = "Manage a per-task todo list";
    e.emoji = "\xe2\x9c\x85";  // check mark
    e.schema = nlohmann::json::parse(R"JSON({
        "type": "object",
        "properties": {
            "todos": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "id": {"type": "string"},
                        "content": {"type": "string"},
                        "status": {
                            "type": "string",
                            "enum": ["pending", "in_progress", "completed", "cancelled"]
                        }
                    }
                },
                "description": "Todo items to set or merge"
            },
            "merge": {
                "type": "boolean",
                "default": false,
                "description": "If true, merge with existing items by id"
            }
        }
    })JSON");

    e.handler = [](const nlohmann::json& args,
                   const ToolContext& ctx) -> std::string {
        std::lock_guard<std::mutex> lk(todos_mu());
        auto& store = todos_store();
        const std::string& task = ctx.task_id;

        // Read mode
        if (!args.contains("todos") || args["todos"].is_null()) {
            auto it = store.find(task);
            if (it == store.end()) {
                return tool_result({{"todos", nlohmann::json::array()},
                                    {"total", 0}});
            }
            return tool_result(
                {{"todos", items_to_json(it->second)},
                 {"total", static_cast<int>(it->second.size())}});
        }

        if (!args["todos"].is_array()) {
            return tool_error("'todos' must be an array");
        }

        // Validate statuses
        for (const auto& obj : args["todos"]) {
            if (obj.contains("status") && obj["status"].is_string()) {
                if (!valid_status(obj["status"].get<std::string>())) {
                    return tool_error(
                        "invalid status: " +
                        obj["status"].get<std::string>() +
                        "; expected pending|in_progress|completed|cancelled");
                }
            }
        }

        auto new_items = parse_todo_items(args["todos"]);
        bool merge = args.value("merge", false);

        if (!merge) {
            // Replace entire list
            store[task] = std::move(new_items);
        } else {
            // Merge: update existing by id, add new ones
            auto& existing = store[task];
            for (auto& ni : new_items) {
                bool found = false;
                for (auto& ei : existing) {
                    if (ei.id == ni.id) {
                        if (!ni.content.empty()) ei.content = ni.content;
                        if (!ni.status.empty()) ei.status = ni.status;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    existing.push_back(std::move(ni));
                }
            }
        }

        auto& final_list = store[task];
        return tool_result(
            {{"todos", items_to_json(final_list)},
             {"total", static_cast<int>(final_list.size())}});
    };

    registry.register_tool(std::move(e));
}

}  // namespace hermes::tools
