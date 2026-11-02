#pragma once
#include "scoped_bindings.hpp"
#include "host_dx12/observe/operation_snapshot.hpp"
namespace na::scale {
// Translate the observer's actual current views into a reduced operation.
// This helper selects no game or scene: the automatic scene owner supplies the
// observed dependency group. Native material views and CBVs remain intact.
bool NativeBindingSpec(const na::observe::NativeOperation&, std::shared_ptr<const SceneGroup>,
                       BindingSpec&, std::string&,bool submission_constants=false);
}
