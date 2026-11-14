#pragma once
#include "jianssh/ui/workspace.hpp"
#include <filesystem>
#include <memory>
namespace jianssh::app {
class WorkspaceController {
public:
 explicit WorkspaceController(std::filesystem::path database);
 ~WorkspaceController();
 WorkspaceController(const WorkspaceController&)=delete;
 const WorkspaceModel& model() const;
 const WorkspaceCallbacks& callbacks() const;
 bool tick();
 bool busy() const;
 bool has_pending_output() const;
 void begin_shutdown();
 bool shutdown_complete() const;
private:
 struct Impl;
 std::unique_ptr<Impl> impl_;
};
}
