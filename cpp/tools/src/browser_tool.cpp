#include "hermes/tools/browser_tool.hpp"
#include "hermes/tools/browser_backend.hpp"
#include "hermes/tools/registry.hpp"

#include <string>

namespace hermes::tools {

namespace {

std::string handle_browser_navigate(const nlohmann::json& args,
                                    const ToolContext& /*ctx*/) {
    auto* b = get_browser_backend();
    if (!b) return tool_error("browser not initialized");
    auto url = args.at("url").get<std::string>();
    b->navigate(url);
    nlohmann::json r;
    r["navigated"] = true;
    r["url"] = url;
    return tool_result(r);
}

std::string handle_browser_snapshot(const nlohmann::json& /*args*/,
                                    const ToolContext& /*ctx*/) {
    auto* b = get_browser_backend();
    if (!b) return tool_error("browser not initialized");
    auto snap = b->snapshot();
    nlohmann::json r;
    r["url"] = snap.url;
    r["title"] = snap.title;
    r["elements"] = nlohmann::json::array();
    for (const auto& el : snap.elements) {
        r["elements"].push_back({
            {"ref", el.ref}, {"tag", el.tag},
            {"text", el.text}, {"role", el.role}
        });
    }
    return tool_result(r);
}

std::string handle_browser_click(const nlohmann::json& args,
                                 const ToolContext& /*ctx*/) {
    auto* b = get_browser_backend();
    if (!b) return tool_error("browser not initialized");
    auto ref = args.at("ref").get<std::string>();
    bool dbl = args.contains("dbl_click") && args["dbl_click"].get<bool>();
    b->click(ref, dbl);
    return tool_result({{"clicked", true}});
}

std::string handle_browser_type(const nlohmann::json& args,
                                const ToolContext& /*ctx*/) {
    auto* b = get_browser_backend();
    if (!b) return tool_error("browser not initialized");
    auto ref = args.at("ref").get<std::string>();
    auto text = args.at("text").get<std::string>();
    bool submit = args.contains("submit") && args["submit"].get<bool>();
    b->type(ref, text, submit);
    return tool_result({{"typed", true}});
}

std::string handle_browser_scroll(const nlohmann::json& args,
                                  const ToolContext& /*ctx*/) {
    auto* b = get_browser_backend();
    if (!b) return tool_error("browser not initialized");
    auto dir = args.at("direction").get<std::string>();
    b->scroll(dir);
    return tool_result({{"scrolled", true}});
}

std::string handle_browser_back(const nlohmann::json& /*args*/,
                                const ToolContext& /*ctx*/) {
    auto* b = get_browser_backend();
    if (!b) return tool_error("browser not initialized");
    b->go_back();
    return tool_result({{"navigated_back", true}});
}

std::string handle_browser_press(const nlohmann::json& args,
                                 const ToolContext& /*ctx*/) {
    auto* b = get_browser_backend();
    if (!b) return tool_error("browser not initialized");
    auto key = args.at("key").get<std::string>();
    b->press_key(key);
    return tool_result({{"pressed", true}});
}

std::string handle_browser_get_images(const nlohmann::json& /*args*/,
                                      const ToolContext& /*ctx*/) {
    auto* b = get_browser_backend();
    if (!b) return tool_error("browser not initialized");
    auto urls = b->get_image_urls();
    nlohmann::json r;
    r["images"] = urls;
    return tool_result(r);
}

std::string handle_browser_vision(const nlohmann::json& args,
                                  const ToolContext& /*ctx*/) {
    auto* b = get_browser_backend();
    if (!b) return tool_error("browser not initialized");
    // prompt is accepted but not used until vision LLM is wired.
    (void)args.at("prompt").get<std::string>();
    auto shot = b->screenshot_base64();
    nlohmann::json r;
    r["analysis"] = "vision not wired";
    r["screenshot_size"] = static_cast<int>(shot.size());
    return tool_result(r);
}

std::string handle_browser_console(const nlohmann::json& args,
                                   const ToolContext& /*ctx*/) {
    auto* b = get_browser_backend();
    if (!b) return tool_error("browser not initialized");
    auto expr = args.at("expression").get<std::string>();
    auto cr = b->evaluate_js(expr);
    nlohmann::json r;
    r["value"] = cr.value;
    r["is_error"] = cr.is_error;
    return tool_result(r);
}

}  // namespace

void register_browser_tools() {
    auto& reg = ToolRegistry::instance();
    auto browser_check = [] { return get_browser_backend() != nullptr; };

    // 1. browser_navigate
    {
        ToolEntry e;
        e.name = "browser_navigate";
        e.toolset = "browser";
        e.description = "Navigate browser to a URL";
        e.schema = {
            {"type", "object"},
            {"properties", {{"url", {{"type", "string"}, {"description", "URL to navigate to"}}}}},
            {"required", nlohmann::json::array({"url"})}};
        e.handler = handle_browser_navigate;
        e.check_fn = browser_check;
        reg.register_tool(std::move(e));
    }

    // 2. browser_snapshot
    {
        ToolEntry e;
        e.name = "browser_snapshot";
        e.toolset = "browser";
        e.description = "Get a snapshot of the current page DOM";
        e.schema = {{"type", "object"}, {"properties", nlohmann::json::object()}};
        e.handler = handle_browser_snapshot;
        e.check_fn = browser_check;
        reg.register_tool(std::move(e));
    }

    // 3. browser_click
    {
        ToolEntry e;
        e.name = "browser_click";
        e.toolset = "browser";
        e.description = "Click an element by ref";
        e.schema = {
            {"type", "object"},
            {"properties", {
                {"ref", {{"type", "string"}, {"description", "Element reference"}}},
                {"dbl_click", {{"type", "boolean"}, {"description", "Double-click (default false)"}}}}},
            {"required", nlohmann::json::array({"ref"})}};
        e.handler = handle_browser_click;
        e.check_fn = browser_check;
        reg.register_tool(std::move(e));
    }

    // 4. browser_type
    {
        ToolEntry e;
        e.name = "browser_type";
        e.toolset = "browser";
        e.description = "Type text into an element";
        e.schema = {
            {"type", "object"},
            {"properties", {
                {"ref", {{"type", "string"}, {"description", "Element reference"}}},
                {"text", {{"type", "string"}, {"description", "Text to type"}}},
                {"submit", {{"type", "boolean"}, {"description", "Submit after typing (default false)"}}}}},
            {"required", nlohmann::json::array({"ref", "text"})}};
        e.handler = handle_browser_type;
        e.check_fn = browser_check;
        reg.register_tool(std::move(e));
    }

    // 5. browser_scroll
    {
        ToolEntry e;
        e.name = "browser_scroll";
        e.toolset = "browser";
        e.description = "Scroll the page in a direction";
        e.schema = {
            {"type", "object"},
            {"properties", {
                {"direction", {{"type", "string"}, {"description", "up|down|left|right"}}}}},
            {"required", nlohmann::json::array({"direction"})}};
        e.handler = handle_browser_scroll;
        e.check_fn = browser_check;
        reg.register_tool(std::move(e));
    }

    // 6. browser_back
    {
        ToolEntry e;
        e.name = "browser_back";
        e.toolset = "browser";
        e.description = "Navigate back in browser history";
        e.schema = {{"type", "object"}, {"properties", nlohmann::json::object()}};
        e.handler = handle_browser_back;
        e.check_fn = browser_check;
        reg.register_tool(std::move(e));
    }

    // 7. browser_press
    {
        ToolEntry e;
        e.name = "browser_press";
        e.toolset = "browser";
        e.description = "Press a keyboard key";
        e.schema = {
            {"type", "object"},
            {"properties", {
                {"key", {{"type", "string"}, {"description", "Key to press, e.g. Enter, Ctrl+A"}}}}},
            {"required", nlohmann::json::array({"key"})}};
        e.handler = handle_browser_press;
        e.check_fn = browser_check;
        reg.register_tool(std::move(e));
    }

    // 8. browser_get_images
    {
        ToolEntry e;
        e.name = "browser_get_images";
        e.toolset = "browser";
        e.description = "Get all image URLs on the page";
        e.schema = {{"type", "object"}, {"properties", nlohmann::json::object()}};
        e.handler = handle_browser_get_images;
        e.check_fn = browser_check;
        reg.register_tool(std::move(e));
    }

    // 9. browser_vision
    {
        ToolEntry e;
        e.name = "browser_vision";
        e.toolset = "browser";
        e.description = "Take screenshot and analyze with vision LLM";
        e.schema = {
            {"type", "object"},
            {"properties", {
                {"prompt", {{"type", "string"}, {"description", "Vision analysis prompt"}}}}},
            {"required", nlohmann::json::array({"prompt"})}};
        e.handler = handle_browser_vision;
        e.check_fn = browser_check;
        reg.register_tool(std::move(e));
    }

    // 10. browser_console
    {
        ToolEntry e;
        e.name = "browser_console";
        e.toolset = "browser";
        e.description = "Evaluate JavaScript in the browser console";
        e.schema = {
            {"type", "object"},
            {"properties", {
                {"expression", {{"type", "string"}, {"description", "JavaScript expression to evaluate"}}}}},
            {"required", nlohmann::json::array({"expression"})}};
        e.handler = handle_browser_console;
        e.check_fn = browser_check;
        reg.register_tool(std::move(e));
    }
}

}  // namespace hermes::tools
