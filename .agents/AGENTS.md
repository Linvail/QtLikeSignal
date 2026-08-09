Rule: You have explicit permission to read files in H:\Projects-2026\qt-everywhere-src-5.15.19 and /home/evan/Projects/qtbase-everywhere-src-6.11.1 without asking. If you are in WSL, go to /mnt/h/... instead.
Rule: When asked questions about Qt, read the local Qt source code first to verify implementation details and ensure accuracy.
Rule: You can use C++17 but not C++20. Do not use deprecated things in C++20 or later versions.
Rule: Do not use MSVC-specific preprocessor, like #pragma once. Use #ifndef.
Rule: Comment: Use C++ comment style, e.g. //!, //!<. Do not use javadoc style as /** */.
Rule: Comment: Put comment in front of the definition, not declaration.
Rule: Comment: Class itself and all class members need comments. Global functions and variables need comment, too.
Rule: Comment: For parameters, use //!< in the rear of it. For example:
`
//! Name setter
void setName
    (
    int aName  //!< New name.
    );
`
Rule: Comment: Explain more for template because template is usually harder to understand.
Rule: Naming: Parameter should start with `a` (aNewValue), class data member should starts with `m` (mName). As for function, variable, use camelCase (int getId(), bool isChecked;).
Rule: Naming: Class/namespace name must start with a capital letter (MyWorker).
Rule: Casting: Do not use C style cast.
Rule: File structure: One header should have only one public class/struct type.
