#pragma once

class Statement {
public:
    virtual ~Statement() = default;
    virtual int getId() const = 0;
};
