#pragma once

#include <QString>

#include <expected>

namespace tt {

template <class T>
using Result = std::expected<T, QString>;

inline std::unexpected<QString> fail(const QString &message)
{
    return std::unexpected<QString>(message);
}

} // namespace tt
