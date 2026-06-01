functions_str = """AllPlanes
BlackPixel
WhitePixel
ConnectionNumber
DefaultColormap
DefaultDepth
DefaultGC
DefaultRootWindow
DefaultScreenOfDisplay
ScreensOfDisplay
DefaultScreen
DefaultVisual
DisplayCells
DisplayPlanes
DisplayString
LastKnownRequestProcessed
NextRequest
ProtocolVersion
ProtocolRevision
QLength
RootWindow
ScreenCount
ServerVendor
VendorRelease"""

functions = set(functions_str.splitlines())

# print(len(functions))
# print(next(iter(functions)))

file1 = open("display-macros.txt", "r")
lines = file1.readlines()
for i in range(0, len(lines)):
    line = lines[i]
    if any(line.startswith(word + "(") for word in functions):
        xfunc_str = lines[i + 2]
        print(xfunc_str.split("(")[0] + "(")
        num_args = 1 + xfunc_str.count(",")
        for j in range(num_args):
            if j == num_args - 1:
                print(lines[i + 2 + j + 1].strip().replace(";", ""))
            else:
                print(lines[i + 2 + j + 1].strip().replace(";", ","))
        print(") { return " + line.strip() + "; }\n")
