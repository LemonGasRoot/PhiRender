#pragma once

// Chart JSON is tens of megabytes of text that cJSON turns into millions of tiny
// nodes; on a big chart malloc/free for those nodes is most of the load time.
// install() points cJSON at a bump arena, so allocation is a pointer add and the
// whole lot is released in one go at process exit.
namespace jsonarena {
void install();
}
