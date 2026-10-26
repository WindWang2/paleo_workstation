# Strict text and comprehensive SpreadsheetML adapters into WellLogRead.
target_sources(paleo_io PRIVATE src/io/welllogtext.cpp src/io/welllogtext_convert.cpp src/io/welllogtext_xml.cpp)
add_paleo_test(tst_welllogtext LIBS paleo_io)
