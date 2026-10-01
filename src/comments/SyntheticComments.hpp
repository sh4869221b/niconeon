#pragma once

#include "domain/Domain.hpp"

namespace niconeon {
bool syntheticCommentModeEnabled();
Result<CommentList> generateSyntheticComments(const QString &videoId);
} // namespace niconeon
