#pragma once

#define C_D(Class)       Class::Private* const d = d_func()
#define C_DP(Class, Ptr) Class::Private* const d = Ptr->d_func()
#define C_Q(Class) Class* const q = q_func()

#define C_DECLARE_PUBLIC(Class, QName)                                              \
    inline Class*       q_func() { return static_cast<Class*>(QName); }             \
    inline const Class* q_func() const { return static_cast<const Class*>(QName); } \
    friend class Class;
