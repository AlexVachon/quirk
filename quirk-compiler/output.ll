; ModuleID = 'ApexCompiler'
source_filename = "ApexCompiler"

%ListIterator = type { %List*, i32 }
%List = type { i8*, i32, i32 }
%String = type { i32, i8* }
%StringIterator = type { %String*, i32 }

@0 = private unnamed_addr constant [49 x i8] c"FATAL ERROR: Allocation failed for ListIterator\0A\00", align 1
@1 = private unnamed_addr constant [58 x i8] c"IndexError: list index out of range (index: %d, len: %d)\0A\00", align 1
@2 = private unnamed_addr constant [48 x i8] c"IndexError: list assignment index out of range\0A\00", align 1
@3 = private unnamed_addr constant [2 x i8] c"[\00", align 1
@4 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@5 = private unnamed_addr constant [3 x i8] c", \00", align 1
@6 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@7 = private unnamed_addr constant [3 x i8] c"%s\00", align 1
@8 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@9 = private unnamed_addr constant [41 x i8] c"FATAL ERROR: Allocation failed for List\0A\00", align 1
@10 = private unnamed_addr constant [2 x i8] c"]\00", align 1
@11 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@12 = private unnamed_addr constant [51 x i8] c"FATAL ERROR: Allocation failed for StringIterator\0A\00", align 1
@13 = private unnamed_addr constant [2 x i8] c"\22\00", align 1
@14 = private unnamed_addr constant [2 x i8] c"\22\00", align 1
@15 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@16 = private unnamed_addr constant [51 x i8] c" this is my string. A String contains characters. \00", align 1
@17 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@18 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@19 = private unnamed_addr constant [4 x i8] c"%f\0A\00", align 1
@20 = private unnamed_addr constant [4 x i8] c"%c\0A\00", align 1
@21 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@22 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@23 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@24 = private unnamed_addr constant [4 x i8] c"%f\0A\00", align 1
@25 = private unnamed_addr constant [4 x i8] c"%c\0A\00", align 1
@26 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@27 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@28 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@29 = private unnamed_addr constant [4 x i8] c"%f\0A\00", align 1
@30 = private unnamed_addr constant [4 x i8] c"%c\0A\00", align 1
@31 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@32 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@33 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@34 = private unnamed_addr constant [4 x i8] c"%f\0A\00", align 1
@35 = private unnamed_addr constant [4 x i8] c"%c\0A\00", align 1
@36 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@37 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@38 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@39 = private unnamed_addr constant [4 x i8] c"%f\0A\00", align 1
@40 = private unnamed_addr constant [4 x i8] c"%c\0A\00", align 1
@41 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@42 = private unnamed_addr constant [42 x i8] c"https://example.com/search?q=hello world!\00", align 1
@43 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@44 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@45 = private unnamed_addr constant [4 x i8] c"%f\0A\00", align 1
@46 = private unnamed_addr constant [4 x i8] c"%c\0A\00", align 1
@47 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@48 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@49 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@50 = private unnamed_addr constant [4 x i8] c"%f\0A\00", align 1
@51 = private unnamed_addr constant [4 x i8] c"%c\0A\00", align 1
@52 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@53 = private unnamed_addr constant [2 x i8] c"i\00", align 1
@54 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@55 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@56 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@57 = private unnamed_addr constant [4 x i8] c"%f\0A\00", align 1
@58 = private unnamed_addr constant [4 x i8] c"%c\0A\00", align 1
@59 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@60 = private unnamed_addr constant [12 x i8] c"characters.\00", align 1
@61 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@62 = private unnamed_addr constant [5 x i8] c"true\00", align 1
@63 = private unnamed_addr constant [6 x i8] c"false\00", align 1
@64 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@65 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@66 = private unnamed_addr constant [4 x i8] c"%f\0A\00", align 1
@67 = private unnamed_addr constant [4 x i8] c"%c\0A\00", align 1
@68 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@69 = private unnamed_addr constant [9 x i8] c"contains\00", align 1
@70 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@71 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@72 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@73 = private unnamed_addr constant [4 x i8] c"%f\0A\00", align 1
@74 = private unnamed_addr constant [4 x i8] c"%c\0A\00", align 1
@75 = private unnamed_addr constant [26 x i8] c"This is simple format: %s\00", align 1
@76 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@77 = private unnamed_addr constant [12 x i8] c"Hello world\00", align 1
@78 = private unnamed_addr constant [41 x i8] c"FATAL ERROR: Allocation failed for List\0A\00", align 1
@79 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@80 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@81 = private unnamed_addr constant [4 x i8] c"%f\0A\00", align 1
@82 = private unnamed_addr constant [4 x i8] c"%c\0A\00", align 1
@83 = private unnamed_addr constant [31 x i8] c"This is %{type} format: %{msg}\00", align 1
@84 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@85 = private unnamed_addr constant [5 x i8] c"type\00", align 1
@86 = private unnamed_addr constant [8 x i8] c"complex\00", align 1
@87 = private unnamed_addr constant [4 x i8] c"msg\00", align 1
@88 = private unnamed_addr constant [12 x i8] c"Hello world\00", align 1
@89 = private unnamed_addr constant [41 x i8] c"FATAL ERROR: Allocation failed for List\0A\00", align 1
@90 = private unnamed_addr constant [41 x i8] c"FATAL ERROR: Allocation failed for List\0A\00", align 1
@91 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@92 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@93 = private unnamed_addr constant [4 x i8] c"%f\0A\00", align 1
@94 = private unnamed_addr constant [4 x i8] c"%c\0A\00", align 1
@95 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@96 = private unnamed_addr constant [7 x i8] c"String\00", align 1
@97 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@98 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@99 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@100 = private unnamed_addr constant [4 x i8] c"%f\0A\00", align 1
@101 = private unnamed_addr constant [4 x i8] c"%c\0A\00", align 1
@102 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@103 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@104 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@105 = private unnamed_addr constant [4 x i8] c"%f\0A\00", align 1
@106 = private unnamed_addr constant [4 x i8] c"%c\0A\00", align 1
@107 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@108 = private unnamed_addr constant [7 x i8] c"String\00", align 1
@109 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@110 = private unnamed_addr constant [7 x i8] c"STRING\00", align 1
@111 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@112 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@113 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@114 = private unnamed_addr constant [4 x i8] c"%f\0A\00", align 1
@115 = private unnamed_addr constant [4 x i8] c"%c\0A\00", align 1
@116 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@117 = private unnamed_addr constant [7 x i8] c"potato\00", align 1
@118 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@119 = private unnamed_addr constant [5 x i8] c"true\00", align 1
@120 = private unnamed_addr constant [6 x i8] c"false\00", align 1
@121 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@122 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@123 = private unnamed_addr constant [4 x i8] c"%f\0A\00", align 1
@124 = private unnamed_addr constant [4 x i8] c"%c\0A\00", align 1
@125 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@126 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@127 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@128 = private unnamed_addr constant [4 x i8] c"%f\0A\00", align 1
@129 = private unnamed_addr constant [4 x i8] c"%c\0A\00", align 1
@130 = private unnamed_addr constant [2 x i8] c"|\00", align 1
@131 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@132 = private unnamed_addr constant [41 x i8] c"FATAL ERROR: Allocation failed for List\0A\00", align 1
@133 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@134 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@135 = private unnamed_addr constant [4 x i8] c"%f\0A\00", align 1
@136 = private unnamed_addr constant [4 x i8] c"%c\0A\00", align 1
@137 = private unnamed_addr constant [3 x i8] c"12\00", align 1
@138 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@139 = private unnamed_addr constant [5 x i8] c"true\00", align 1
@140 = private unnamed_addr constant [6 x i8] c"false\00", align 1
@141 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@142 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@143 = private unnamed_addr constant [4 x i8] c"%f\0A\00", align 1
@144 = private unnamed_addr constant [4 x i8] c"%c\0A\00", align 1
@145 = private unnamed_addr constant [3 x i8] c"12\00", align 1
@146 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@147 = private unnamed_addr constant [5 x i8] c"true\00", align 1
@148 = private unnamed_addr constant [6 x i8] c"false\00", align 1
@149 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@150 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@151 = private unnamed_addr constant [4 x i8] c"%f\0A\00", align 1
@152 = private unnamed_addr constant [4 x i8] c"%c\0A\00", align 1
@153 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@154 = private unnamed_addr constant [2 x i8] c"l\00", align 1
@155 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@156 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@157 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@158 = private unnamed_addr constant [4 x i8] c"%f\0A\00", align 1
@159 = private unnamed_addr constant [4 x i8] c"%c\0A\00", align 1
@160 = private unnamed_addr constant [2 x i8] c"-\00", align 1
@161 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@162 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@163 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@164 = private unnamed_addr constant [4 x i8] c"%f\0A\00", align 1
@165 = private unnamed_addr constant [4 x i8] c"%c\0A\00", align 1
@166 = private unnamed_addr constant [9 x i8] c"stressed\00", align 1
@167 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@168 = private unnamed_addr constant [3 x i8] c"42\00", align 1
@169 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@170 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@171 = private unnamed_addr constant [4 x i8] c"%f\0A\00", align 1
@172 = private unnamed_addr constant [4 x i8] c"%c\0A\00", align 1
@173 = private unnamed_addr constant [5 x i8] c"ID: \00", align 1
@174 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@175 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@176 = private unnamed_addr constant [5 x i8] c"Name\00", align 1
@177 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@178 = private unnamed_addr constant [2 x i8] c" \00", align 1
@179 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@180 = private unnamed_addr constant [6 x i8] c"Price\00", align 1
@181 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@182 = private unnamed_addr constant [2 x i8] c" \00", align 1
@183 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@184 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@185 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@186 = private unnamed_addr constant [4 x i8] c"%f\0A\00", align 1
@187 = private unnamed_addr constant [4 x i8] c"%c\0A\00", align 1
@188 = private unnamed_addr constant [2 x i8] c"|\00", align 1
@189 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@190 = private unnamed_addr constant [6 x i8] c"Apple\00", align 1
@191 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@192 = private unnamed_addr constant [2 x i8] c".\00", align 1
@193 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@194 = private unnamed_addr constant [6 x i8] c"$1.50\00", align 1
@195 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@196 = private unnamed_addr constant [2 x i8] c".\00", align 1
@197 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@198 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@199 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@200 = private unnamed_addr constant [4 x i8] c"%f\0A\00", align 1
@201 = private unnamed_addr constant [4 x i8] c"%c\0A\00", align 1
@202 = private unnamed_addr constant [2 x i8] c"|\00", align 1
@203 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@204 = private unnamed_addr constant [5 x i8] c"Team\00", align 1
@205 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@206 = private unnamed_addr constant [3 x i8] c"ea\00", align 1
@207 = private unnamed_addr constant [43 x i8] c"FATAL ERROR: Allocation failed for String\0A\00", align 1
@208 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@209 = private unnamed_addr constant [4 x i8] c"%s\0A\00", align 1
@210 = private unnamed_addr constant [4 x i8] c"%f\0A\00", align 1
@211 = private unnamed_addr constant [4 x i8] c"%c\0A\00", align 1
@212 = private unnamed_addr constant [10 x i8] c"Found it!\00", align 1

declare i32 @printf(i8*, ...)

declare i8* @malloc(i64)

declare void @free(i8*)

declare i32 @sprintf(i8*, i8*, ...)

declare i64 @strlen(i8*)

declare i8* @strcpy(i8*, i8*)

declare i8* @strcat(i8*, i8*)

declare i32 @printf.1(i8*, i8*)

declare i32 @sprintf.2(i8*, i8*, i8*)

declare i8* @malloc.3(i32)

declare i8* @realloc(i8*, i32)

declare void @free.4(i8*)

declare void @exit(i32)

declare i32 @strlen.5(i8*)

declare i8* @strcpy.6(i8*, i8*)

declare i8* @strcat.7(i8*, i8*)

declare i32 @strcmp(i8*, i8*)

define void @ListIterator__init(%ListIterator* %self, %List* %l) {
entry:
  %l2 = alloca %List*
  %self1 = alloca %ListIterator*
  store %ListIterator* %self, %ListIterator** %self1
  store %List* %l, %List** %l2
  %l3 = load %List*, %List** %l2
  %self4 = load %ListIterator*, %ListIterator** %self1
  %0 = getelementptr inbounds %ListIterator, %ListIterator* %self4, i32 0, i32 0
  store %List* %l3, %List** %0
  %self5 = load %ListIterator*, %ListIterator** %self1
  %1 = getelementptr inbounds %ListIterator, %ListIterator* %self5, i32 0, i32 1
  store i32 0, i32* %1
  ret void
}

define i1 @ListIterator___has_next(%ListIterator* %self) {
entry:
  %self1 = alloca %ListIterator*
  store %ListIterator* %self, %ListIterator** %self1
  %self2 = load %ListIterator*, %ListIterator** %self1
  %0 = getelementptr inbounds %ListIterator, %ListIterator* %self2, i32 0, i32 1
  %1 = load i32, i32* %0
  %self3 = load %ListIterator*, %ListIterator** %self1
  %2 = getelementptr inbounds %ListIterator, %ListIterator* %self3, i32 0, i32 0
  %3 = load %List*, %List** %2
  %4 = call i32 @List_len(%List* %3)
  %i_lt = icmp slt i32 %1, %4
  ret i1 %i_lt
}

define i8* @ListIterator___next(%ListIterator* %self) {
entry:
  %val = alloca i8*
  %self1 = alloca %ListIterator*
  store %ListIterator* %self, %ListIterator** %self1
  %self2 = load %ListIterator*, %ListIterator** %self1
  %0 = getelementptr inbounds %ListIterator, %ListIterator* %self2, i32 0, i32 0
  %1 = load %List*, %List** %0
  %self3 = load %ListIterator*, %ListIterator** %self1
  %2 = getelementptr inbounds %ListIterator, %ListIterator* %self3, i32 0, i32 1
  %3 = load i32, i32* %2
  %4 = call i8* @List___get(%List* %1, i32 %3)
  store i8* %4, i8** %val
  %self4 = load %ListIterator*, %ListIterator** %self1
  %5 = getelementptr inbounds %ListIterator, %ListIterator* %self4, i32 0, i32 1
  %6 = load i32, i32* %5
  %i_add = add i32 %6, 1
  %self5 = load %ListIterator*, %ListIterator** %self1
  %7 = getelementptr inbounds %ListIterator, %ListIterator* %self5, i32 0, i32 1
  store i32 %i_add, i32* %7
  %val6 = load i8*, i8** %val
  ret i8* %val6
}

define void @List__init(%List* %self, i32 %initial_cap) {
entry:
  %initial_cap2 = alloca i32
  %self1 = alloca %List*
  store %List* %self, %List** %self1
  store i32 %initial_cap, i32* %initial_cap2
  %initial_cap3 = load i32, i32* %initial_cap2
  %i_lt = icmp slt i32 %initial_cap3, 1
  br i1 %i_lt, label %then, label %ifcont

then:                                             ; preds = %entry
  store i32 1, i32* %initial_cap2
  br label %ifcont

ifcont:                                           ; preds = %then, %entry
  %initial_cap4 = load i32, i32* %initial_cap2
  %self5 = load %List*, %List** %self1
  %0 = getelementptr inbounds %List, %List* %self5, i32 0, i32 2
  store i32 %initial_cap4, i32* %0
  %self6 = load %List*, %List** %self1
  %1 = getelementptr inbounds %List, %List* %self6, i32 0, i32 1
  store i32 0, i32* %1
  %self7 = load %List*, %List** %self1
  %2 = getelementptr inbounds %List, %List* %self7, i32 0, i32 2
  %3 = load i32, i32* %2
  %i_mul = mul i32 %3, 8
  %4 = sext i32 %i_mul to i64
  %5 = call i8* @malloc(i64 %4)
  %self8 = load %List*, %List** %self1
  %6 = getelementptr inbounds %List, %List* %self8, i32 0, i32 0
  store i8* %5, i8** %6
  ret void
}

define void @List_append(%List* %self, i8* %item) {
entry:
  %item2 = alloca i8*
  %self1 = alloca %List*
  store %List* %self, %List** %self1
  store i8* %item, i8** %item2
  %self3 = load %List*, %List** %self1
  %0 = getelementptr inbounds %List, %List* %self3, i32 0, i32 1
  %1 = load i32, i32* %0
  %self4 = load %List*, %List** %self1
  %2 = getelementptr inbounds %List, %List* %self4, i32 0, i32 2
  %3 = load i32, i32* %2
  %i_eq = icmp eq i32 %1, %3
  br i1 %i_eq, label %then, label %ifcont

then:                                             ; preds = %entry
  %self5 = load %List*, %List** %self1
  %self6 = load %List*, %List** %self1
  %4 = getelementptr inbounds %List, %List* %self6, i32 0, i32 2
  %5 = load i32, i32* %4
  %i_mul = mul i32 %5, 2
  call void @List__resize(%List* %self5, i32 %i_mul)
  br label %ifcont

ifcont:                                           ; preds = %then, %entry
  %item7 = load i8*, i8** %item2
  %self8 = load %List*, %List** %self1
  %6 = getelementptr inbounds %List, %List* %self8, i32 0, i32 0
  %7 = load i8*, i8** %6
  %self9 = load %List*, %List** %self1
  %8 = getelementptr inbounds %List, %List* %self9, i32 0, i32 1
  %9 = load i32, i32* %8
  %10 = bitcast i8* %7 to i8**
  %11 = getelementptr i8*, i8** %10, i32 %9
  store i8* %item7, i8** %11
  %self10 = load %List*, %List** %self1
  %12 = getelementptr inbounds %List, %List* %self10, i32 0, i32 1
  %13 = load i32, i32* %12
  %i_add = add i32 %13, 1
  %self11 = load %List*, %List** %self1
  %14 = getelementptr inbounds %List, %List* %self11, i32 0, i32 1
  store i32 %i_add, i32* %14
  ret void
}

define i8* @List_pop(%List* %self) {
entry:
  %self1 = alloca %List*
  store %List* %self, %List** %self1
  %self2 = load %List*, %List** %self1
  %0 = getelementptr inbounds %List, %List* %self2, i32 0, i32 1
  %1 = load i32, i32* %0
  %i_eq = icmp eq i32 %1, 0
  br i1 %i_eq, label %then, label %ifcont

then:                                             ; preds = %entry
  ret i8* null

ifcont:                                           ; preds = %entry
  %self3 = load %List*, %List** %self1
  %2 = getelementptr inbounds %List, %List* %self3, i32 0, i32 1
  %3 = load i32, i32* %2
  %i_sub = sub i32 %3, 1
  %self4 = load %List*, %List** %self1
  %4 = getelementptr inbounds %List, %List* %self4, i32 0, i32 1
  store i32 %i_sub, i32* %4
  %self5 = load %List*, %List** %self1
  %5 = getelementptr inbounds %List, %List* %self5, i32 0, i32 0
  %6 = load i8*, i8** %5
  %self6 = load %List*, %List** %self1
  %7 = getelementptr inbounds %List, %List* %self6, i32 0, i32 1
  %8 = load i32, i32* %7
  %9 = bitcast i8* %6 to i8**
  %10 = getelementptr i8*, i8** %9, i32 %8
  %11 = load i8*, i8** %10
  ret i8* %11
}

define i32 @List_len(%List* %self) {
entry:
  %self1 = alloca %List*
  store %List* %self, %List** %self1
  %self2 = load %List*, %List** %self1
  %0 = getelementptr inbounds %List, %List* %self2, i32 0, i32 1
  %1 = load i32, i32* %0
  ret i32 %1
}

define void @List_clear(%List* %self) {
entry:
  %self1 = alloca %List*
  store %List* %self, %List** %self1
  %self2 = load %List*, %List** %self1
  %0 = getelementptr inbounds %List, %List* %self2, i32 0, i32 1
  store i32 0, i32* %0
  ret void
}

define i1 @List_is_empty(%List* %self) {
entry:
  %self1 = alloca %List*
  store %List* %self, %List** %self1
  %self2 = load %List*, %List** %self1
  %0 = getelementptr inbounds %List, %List* %self2, i32 0, i32 1
  %1 = load i32, i32* %0
  %i_eq = icmp eq i32 %1, 0
  ret i1 %i_eq
}

define void @List__resize(%List* %self, i32 %new_cap) {
entry:
  %new_cap2 = alloca i32
  %self1 = alloca %List*
  store %List* %self, %List** %self1
  store i32 %new_cap, i32* %new_cap2
  %new_cap3 = load i32, i32* %new_cap2
  %self4 = load %List*, %List** %self1
  %0 = getelementptr inbounds %List, %List* %self4, i32 0, i32 2
  store i32 %new_cap3, i32* %0
  %self5 = load %List*, %List** %self1
  %1 = getelementptr inbounds %List, %List* %self5, i32 0, i32 0
  %2 = load i8*, i8** %1
  %new_cap6 = load i32, i32* %new_cap2
  %i_mul = mul i32 %new_cap6, 8
  %3 = call i8* @realloc(i8* %2, i32 %i_mul)
  %self7 = load %List*, %List** %self1
  %4 = getelementptr inbounds %List, %List* %self7, i32 0, i32 0
  store i8* %3, i8** %4
  ret void
}

define void @List___del(%List* %self) {
entry:
  %self1 = alloca %List*
  store %List* %self, %List** %self1
  %self2 = load %List*, %List** %self1
  %0 = getelementptr inbounds %List, %List* %self2, i32 0, i32 0
  %1 = load i8*, i8** %0
  call void @free(i8* %1)
  ret void
}

define %ListIterator* @List___iter(%List* %self) {
entry:
  %self1 = alloca %List*
  store %List* %self, %List** %self1
  %self2 = load %List*, %List** %self1
  %0 = call i8* @malloc(i64 16)
  %1 = icmp eq i8* %0, null
  br i1 %1, label %mem_panic, label %mem_ok

mem_panic:                                        ; preds = %entry
  %2 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([49 x i8], [49 x i8]* @0, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok:                                           ; preds = %entry
  %3 = bitcast i8* %0 to %ListIterator*
  call void @ListIterator__init(%ListIterator* %3, %List* %self2)
  ret %ListIterator* %3
}

define i8* @List___get(%List* %self, i32 %index) {
entry:
  %index2 = alloca i32
  %self1 = alloca %List*
  store %List* %self, %List** %self1
  store i32 %index, i32* %index2
  %index3 = load i32, i32* %index2
  %i_lt = icmp slt i32 %index3, 0
  br i1 %i_lt, label %then, label %ifcont

then:                                             ; preds = %entry
  %self4 = load %List*, %List** %self1
  %0 = getelementptr inbounds %List, %List* %self4, i32 0, i32 1
  %1 = load i32, i32* %0
  %index5 = load i32, i32* %index2
  %i_add = add i32 %1, %index5
  store i32 %i_add, i32* %index2
  br label %ifcont

ifcont:                                           ; preds = %then, %entry
  %index7 = load i32, i32* %index2
  %i_lt8 = icmp slt i32 %index7, 0
  %index9 = load i32, i32* %index2
  %self10 = load %List*, %List** %self1
  %2 = getelementptr inbounds %List, %List* %self10, i32 0, i32 1
  %3 = load i32, i32* %2
  %i_ge = icmp sge i32 %index9, %3
  %or_result = or i1 %i_lt8, %i_ge
  br i1 %or_result, label %then6, label %ifcont13

then6:                                            ; preds = %ifcont
  %index11 = load i32, i32* %index2
  %4 = inttoptr i32 %index11 to i8*
  %self12 = load %List*, %List** %self1
  %5 = getelementptr inbounds %List, %List* %self12, i32 0, i32 1
  %6 = load i32, i32* %5
  %7 = inttoptr i32 %6 to i8*
  %8 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([58 x i8], [58 x i8]* @1, i32 0, i32 0))
  call void @exit(i32 1)
  br label %ifcont13

ifcont13:                                         ; preds = %then6, %ifcont
  %self14 = load %List*, %List** %self1
  %9 = getelementptr inbounds %List, %List* %self14, i32 0, i32 0
  %10 = load i8*, i8** %9
  %index15 = load i32, i32* %index2
  %11 = bitcast i8* %10 to i8**
  %12 = getelementptr i8*, i8** %11, i32 %index15
  %13 = load i8*, i8** %12
  ret i8* %13
}

define void @List___set(%List* %self, i32 %index, i8* %item) {
entry:
  %item3 = alloca i8*
  %index2 = alloca i32
  %self1 = alloca %List*
  store %List* %self, %List** %self1
  store i32 %index, i32* %index2
  store i8* %item, i8** %item3
  %index4 = load i32, i32* %index2
  %i_lt = icmp slt i32 %index4, 0
  br i1 %i_lt, label %then, label %ifcont

then:                                             ; preds = %entry
  %self5 = load %List*, %List** %self1
  %0 = getelementptr inbounds %List, %List* %self5, i32 0, i32 1
  %1 = load i32, i32* %0
  %index6 = load i32, i32* %index2
  %i_add = add i32 %1, %index6
  store i32 %i_add, i32* %index2
  br label %ifcont

ifcont:                                           ; preds = %then, %entry
  %index8 = load i32, i32* %index2
  %i_lt9 = icmp slt i32 %index8, 0
  %index10 = load i32, i32* %index2
  %self11 = load %List*, %List** %self1
  %2 = getelementptr inbounds %List, %List* %self11, i32 0, i32 1
  %3 = load i32, i32* %2
  %i_ge = icmp sge i32 %index10, %3
  %or_result = or i1 %i_lt9, %i_ge
  br i1 %or_result, label %then7, label %ifcont12

then7:                                            ; preds = %ifcont
  %4 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([48 x i8], [48 x i8]* @2, i32 0, i32 0))
  call void @exit(i32 1)
  br label %ifcont12

ifcont12:                                         ; preds = %then7, %ifcont
  %item13 = load i8*, i8** %item3
  %self14 = load %List*, %List** %self1
  %5 = getelementptr inbounds %List, %List* %self14, i32 0, i32 0
  %6 = load i8*, i8** %5
  %index15 = load i32, i32* %index2
  %7 = bitcast i8* %6 to i8**
  %8 = getelementptr i8*, i8** %7, i32 %index15
  store i8* %item13, i8** %8
  ret void
}

define %String* @List___str(%List* %self) {
entry:
  %self1 = alloca %List*
  store %List* %self, %List** %self1
  %self2 = load %List*, %List** %self1
  %0 = call %String* @List___repr(%List* %self2)
  ret %String* %0
}

define %String* @List___repr(%List* %self) {
entry:
  %item3 = alloca i8*
  %i = alloca i32
  %s = alloca %String*
  %self1 = alloca %List*
  store %List* %self, %List** %self1
  %0 = call i8* @malloc(i64 16)
  %1 = icmp eq i8* %0, null
  br i1 %1, label %mem_panic, label %mem_ok

mem_panic:                                        ; preds = %entry
  %2 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @4, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok:                                           ; preds = %entry
  %3 = bitcast i8* %0 to %String*
  call void @String__init(%String* %3, i8* getelementptr inbounds ([2 x i8], [2 x i8]* @3, i32 0, i32 0))
  store %String* %3, %String** %s
  store i32 0, i32* %i
  %self2 = load %List*, %List** %self1
  %iter = call %ListIterator* @List___iter(%List* %self2)
  br label %forcond

forcond:                                          ; preds = %mem_ok13, %mem_ok
  %has_next = call i1 @ListIterator___has_next(%ListIterator* %iter)
  br i1 %has_next, label %forbody, label %forafter

forbody:                                          ; preds = %forcond
  %item = call i8* @ListIterator___next(%ListIterator* %iter)
  store i8* %item, i8** %item3
  %i4 = load i32, i32* %i
  %i_gt = icmp sgt i32 %i4, 0
  br i1 %i_gt, label %then, label %ifcont

forafter:                                         ; preds = %forcond
  %s15 = load %String*, %String** %s
  %4 = call i8* @malloc(i64 16)
  %5 = icmp eq i8* %4, null
  br i1 %5, label %mem_panic16, label %mem_ok17

then:                                             ; preds = %forbody
  %s5 = load %String*, %String** %s
  %6 = call i8* @malloc(i64 16)
  %7 = icmp eq i8* %6, null
  br i1 %7, label %mem_panic6, label %mem_ok7

mem_panic6:                                       ; preds = %then
  %8 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @6, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok7:                                          ; preds = %then
  %9 = bitcast i8* %6 to %String*
  call void @String__init(%String* %9, i8* getelementptr inbounds ([3 x i8], [3 x i8]* @5, i32 0, i32 0))
  %10 = call %String* @String___add(%String* %s5, %String* %9)
  store %String* %10, %String** %s
  br label %ifcont

ifcont:                                           ; preds = %mem_ok7, %forbody
  %s8 = load %String*, %String** %s
  %11 = call i8* @malloc(i64 16)
  %12 = icmp eq i8* %11, null
  br i1 %12, label %mem_panic9, label %mem_ok10

mem_panic9:                                       ; preds = %ifcont
  %13 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @8, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok10:                                         ; preds = %ifcont
  %14 = bitcast i8* %11 to %String*
  call void @String__init(%String* %14, i8* getelementptr inbounds ([3 x i8], [3 x i8]* @7, i32 0, i32 0))
  %item11 = load i8*, i8** %item3
  %15 = call i8* @malloc(i64 16)
  %16 = icmp eq i8* %15, null
  br i1 %16, label %mem_panic12, label %mem_ok13

mem_panic12:                                      ; preds = %mem_ok10
  %17 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([41 x i8], [41 x i8]* @9, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok13:                                         ; preds = %mem_ok10
  %18 = bitcast i8* %15 to %List*
  call void @List__init(%List* %18, i32 1)
  %19 = getelementptr inbounds %List, %List* %18, i32 0, i32 0
  %20 = load i8*, i8** %19
  %21 = bitcast i8* %20 to i8**
  %22 = getelementptr i8*, i8** %21, i32 0
  store i8* %item11, i8** %22
  %23 = getelementptr inbounds %List, %List* %18, i32 0, i32 1
  store i32 1, i32* %23
  %24 = getelementptr inbounds %List, %List* %18, i32 0, i32 2
  store i32 1, i32* %24
  %25 = call %String* @String_format_list(%String* %14, %List* %18)
  %26 = call %String* @String___add(%String* %s8, %String* %25)
  store %String* %26, %String** %s
  %i14 = load i32, i32* %i
  %i_add = add i32 %i14, 1
  store i32 %i_add, i32* %i
  br label %forcond

mem_panic16:                                      ; preds = %forafter
  %27 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @11, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok17:                                         ; preds = %forafter
  %28 = bitcast i8* %4 to %String*
  call void @String__init(%String* %28, i8* getelementptr inbounds ([2 x i8], [2 x i8]* @10, i32 0, i32 0))
  %29 = call %String* @String___add(%String* %s15, %String* %28)
  store %String* %29, %String** %s
  %s18 = load %String*, %String** %s
  ret %String* %s18
}

define void @StringIterator__init(%StringIterator* %self, %String* %s) {
entry:
  %s2 = alloca %String*
  %self1 = alloca %StringIterator*
  store %StringIterator* %self, %StringIterator** %self1
  store %String* %s, %String** %s2
  %s3 = load %String*, %String** %s2
  %self4 = load %StringIterator*, %StringIterator** %self1
  %0 = getelementptr inbounds %StringIterator, %StringIterator* %self4, i32 0, i32 0
  store %String* %s3, %String** %0
  %self5 = load %StringIterator*, %StringIterator** %self1
  %1 = getelementptr inbounds %StringIterator, %StringIterator* %self5, i32 0, i32 1
  store i32 0, i32* %1
  ret void
}

define i1 @StringIterator___has_next(%StringIterator* %self) {
entry:
  %self1 = alloca %StringIterator*
  store %StringIterator* %self, %StringIterator** %self1
  %self2 = load %StringIterator*, %StringIterator** %self1
  %0 = getelementptr inbounds %StringIterator, %StringIterator* %self2, i32 0, i32 1
  %1 = load i32, i32* %0
  %self3 = load %StringIterator*, %StringIterator** %self1
  %2 = getelementptr inbounds %StringIterator, %StringIterator* %self3, i32 0, i32 0
  %3 = load %String*, %String** %2
  %4 = getelementptr inbounds %String, %String* %3, i32 0, i32 0
  %5 = load i32, i32* %4
  %i_lt = icmp slt i32 %1, %5
  ret i1 %i_lt
}

define i8 @StringIterator___next(%StringIterator* %self) {
entry:
  %val = alloca i32
  %self1 = alloca %StringIterator*
  store %StringIterator* %self, %StringIterator** %self1
  %self2 = load %StringIterator*, %StringIterator** %self1
  %0 = getelementptr inbounds %StringIterator, %StringIterator* %self2, i32 0, i32 0
  %1 = load %String*, %String** %0
  %2 = getelementptr inbounds %String, %String* %1, i32 0, i32 1
  %3 = load i8*, i8** %2
  %self3 = load %StringIterator*, %StringIterator** %self1
  %4 = getelementptr inbounds %StringIterator, %StringIterator* %self3, i32 0, i32 1
  %5 = load i32, i32* %4
  %6 = getelementptr i8, i8* %3, i32 %5
  %7 = load i8, i8* %6
  %8 = zext i8 %7 to i32
  store i32 %8, i32* %val
  %self4 = load %StringIterator*, %StringIterator** %self1
  %9 = getelementptr inbounds %StringIterator, %StringIterator* %self4, i32 0, i32 1
  %10 = load i32, i32* %9
  %i_add = add i32 %10, 1
  %self5 = load %StringIterator*, %StringIterator** %self1
  %11 = getelementptr inbounds %StringIterator, %StringIterator* %self5, i32 0, i32 1
  store i32 %i_add, i32* %11
  %val6 = load i32, i32* %val
  %12 = trunc i32 %val6 to i8
  ret i8 %12
}

define void @String__init(%String* %self, i8* %raw) {
entry:
  %raw2 = alloca i8*
  %self1 = alloca %String*
  store %String* %self, %String** %self1
  store i8* %raw, i8** %raw2
  %raw3 = load i8*, i8** %raw2
  %0 = call i64 @strlen(i8* %raw3)
  %self4 = load %String*, %String** %self1
  %1 = getelementptr inbounds %String, %String* %self4, i32 0, i32 0
  %2 = trunc i64 %0 to i32
  store i32 %2, i32* %1
  %self5 = load %String*, %String** %self1
  %3 = getelementptr inbounds %String, %String* %self5, i32 0, i32 0
  %4 = load i32, i32* %3
  %i_add = add i32 %4, 1
  %5 = sext i32 %i_add to i64
  %6 = call i8* @malloc(i64 %5)
  %self6 = load %String*, %String** %self1
  %7 = getelementptr inbounds %String, %String* %self6, i32 0, i32 1
  store i8* %6, i8** %7
  %self7 = load %String*, %String** %self1
  %8 = getelementptr inbounds %String, %String* %self7, i32 0, i32 1
  %9 = load i8*, i8** %8
  %raw8 = load i8*, i8** %raw2
  %10 = call i8* @strcpy(i8* %9, i8* %raw8)
  ret void
}

declare %String* @String_format(%String*, %List*)

declare %String* @String_format_map(%String*, %List*, %List*)

declare %String* @String_format_list(%String*, %List*)

declare %List* @String_split(%String*, %String*)

declare %String* @String_join(%String*, %List*)

declare %String* @String_substring(%String*, i32, i32)

declare %String* @String_upper(%String*)

declare %String* @String_lower(%String*)

declare %String* @String_title(%String*)

declare %String* @String_capitalize(%String*)

declare %String* @String_sentence_case(%String*)

declare %String* @String_encode(%String*)

declare i32 @String_count(%String*, %String*)

declare i1 @String_startswith(%String*, %String*)

declare i1 @String_endswith(%String*, %String*)

declare i32 @String_find(%String*, %String*)

declare i32 @String_index(%String*, %String*)

declare %String* @String_replace(%String*, %String*, %String*)

declare %String* @String_remove(%String*, %String*)

declare %String* @String_repeat(%String*, i32)

declare %String* @String_reverse(%String*)

declare %String* @String_zfill(%String*, i32)

declare %String* @String_ljust(%String*, i32, %String*)

declare %String* @String_rjust(%String*, i32, %String*)

declare %String* @String_center(%String*, i32, %String*)

declare %String* @String_trim(%String*)

declare %String* @String_lstrip(%String*)

declare %String* @String_rstrip(%String*)

declare %String* @String_swapcase(%String*)

declare i1 @String_isalpha(%String*)

declare i1 @String_isdigit(%String*)

declare i1 @String_isspace(%String*)

declare i1 @String_contains(%String*, %String*)

define void @String___del(%String* %self) {
entry:
  %self1 = alloca %String*
  store %String* %self, %String** %self1
  %self2 = load %String*, %String** %self1
  %0 = getelementptr inbounds %String, %String* %self2, i32 0, i32 1
  %1 = load i8*, i8** %0
  call void @free(i8* %1)
  ret void
}

define %StringIterator* @String___iter(%String* %self) {
entry:
  %self1 = alloca %String*
  store %String* %self, %String** %self1
  %self2 = load %String*, %String** %self1
  %0 = call i8* @malloc(i64 16)
  %1 = icmp eq i8* %0, null
  br i1 %1, label %mem_panic, label %mem_ok

mem_panic:                                        ; preds = %entry
  %2 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([51 x i8], [51 x i8]* @12, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok:                                           ; preds = %entry
  %3 = bitcast i8* %0 to %StringIterator*
  call void @StringIterator__init(%StringIterator* %3, %String* %self2)
  ret %StringIterator* %3
}

define i8* @String___str(%String* %self) {
entry:
  %self1 = alloca %String*
  store %String* %self, %String** %self1
  %self2 = load %String*, %String** %self1
  %0 = getelementptr inbounds %String, %String* %self2, i32 0, i32 1
  %1 = load i8*, i8** %0
  ret i8* %1
}

define i8* @String___repr(%String* %self) {
entry:
  %self1 = alloca %String*
  store %String* %self, %String** %self1
  %self2 = load %String*, %String** %self1
  %0 = getelementptr inbounds %String, %String* %self2, i32 0, i32 1
  %1 = load i8*, i8** %0
  %len1 = call i64 @strlen(i8* getelementptr inbounds ([2 x i8], [2 x i8]* @13, i32 0, i32 0))
  %len2 = call i64 @strlen(i8* %1)
  %totalLen = add i64 %len1, %len2
  %2 = add i64 %totalLen, 1
  %newStrRaw = call i8* @malloc(i64 %2)
  %3 = call i8* @strcpy(i8* %newStrRaw, i8* getelementptr inbounds ([2 x i8], [2 x i8]* @13, i32 0, i32 0))
  %4 = call i8* @strcat(i8* %newStrRaw, i8* %1)
  %len13 = call i64 @strlen(i8* %newStrRaw)
  %len24 = call i64 @strlen(i8* getelementptr inbounds ([2 x i8], [2 x i8]* @14, i32 0, i32 0))
  %totalLen5 = add i64 %len13, %len24
  %5 = add i64 %totalLen5, 1
  %newStrRaw6 = call i8* @malloc(i64 %5)
  %6 = call i8* @strcpy(i8* %newStrRaw6, i8* %newStrRaw)
  %7 = call i8* @strcat(i8* %newStrRaw6, i8* getelementptr inbounds ([2 x i8], [2 x i8]* @14, i32 0, i32 0))
  ret i8* %newStrRaw6
}

define %String* @String___add(%String* %self, %String* %other) {
entry:
  %res = alloca %String*
  %raw = alloca i8*
  %newLen = alloca i32
  %other2 = alloca %String*
  %self1 = alloca %String*
  store %String* %self, %String** %self1
  store %String* %other, %String** %other2
  %self3 = load %String*, %String** %self1
  %0 = getelementptr inbounds %String, %String* %self3, i32 0, i32 0
  %1 = load i32, i32* %0
  %other4 = load %String*, %String** %other2
  %2 = getelementptr inbounds %String, %String* %other4, i32 0, i32 0
  %3 = load i32, i32* %2
  %i_add = add i32 %1, %3
  store i32 %i_add, i32* %newLen
  %newLen5 = load i32, i32* %newLen
  %i_add6 = add i32 %newLen5, 1
  %4 = sext i32 %i_add6 to i64
  %5 = call i8* @malloc(i64 %4)
  store i8* %5, i8** %raw
  %raw7 = load i8*, i8** %raw
  %self8 = load %String*, %String** %self1
  %6 = getelementptr inbounds %String, %String* %self8, i32 0, i32 1
  %7 = load i8*, i8** %6
  %8 = call i8* @strcpy(i8* %raw7, i8* %7)
  %raw9 = load i8*, i8** %raw
  %other10 = load %String*, %String** %other2
  %9 = getelementptr inbounds %String, %String* %other10, i32 0, i32 1
  %10 = load i8*, i8** %9
  %11 = call i8* @strcat(i8* %raw9, i8* %10)
  %raw11 = load i8*, i8** %raw
  %12 = call i8* @malloc(i64 16)
  %13 = icmp eq i8* %12, null
  br i1 %13, label %mem_panic, label %mem_ok

mem_panic:                                        ; preds = %entry
  %14 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @15, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok:                                           ; preds = %entry
  %15 = bitcast i8* %12 to %String*
  call void @String__init(%String* %15, i8* %raw11)
  store %String* %15, %String** %res
  %raw12 = load i8*, i8** %raw
  call void @free(i8* %raw12)
  %res13 = load %String*, %String** %res
  ret %String* %res13
}

define i1 @String___eq(%String* %self, %String* %other) {
entry:
  %other2 = alloca %String*
  %self1 = alloca %String*
  store %String* %self, %String** %self1
  store %String* %other, %String** %other2
  %self3 = load %String*, %String** %self1
  %0 = getelementptr inbounds %String, %String* %self3, i32 0, i32 0
  %1 = load i32, i32* %0
  %other4 = load %String*, %String** %other2
  %2 = getelementptr inbounds %String, %String* %other4, i32 0, i32 0
  %3 = load i32, i32* %2
  %i_ne = icmp ne i32 %1, %3
  br i1 %i_ne, label %then, label %ifcont

then:                                             ; preds = %entry
  ret i1 false

ifcont:                                           ; preds = %entry
  %self5 = load %String*, %String** %self1
  %4 = getelementptr inbounds %String, %String* %self5, i32 0, i32 1
  %5 = load i8*, i8** %4
  %other6 = load %String*, %String** %other2
  %6 = getelementptr inbounds %String, %String* %other6, i32 0, i32 1
  %7 = load i8*, i8** %6
  %8 = call i32 @strcmp(i8* %5, i8* %7)
  %i_eq = icmp eq i32 %8, 0
  ret i1 %i_eq
}

define i1 @String_empty(%String* %self) {
entry:
  %self1 = alloca %String*
  store %String* %self, %String** %self1
  %self2 = load %String*, %String** %self1
  %0 = getelementptr inbounds %String, %String* %self2, i32 0, i32 0
  %1 = load i32, i32* %0
  %i_eq = icmp eq i32 %1, 0
  ret i1 %i_eq
}

define i32 @main() {
entry:
  %val2 = alloca %String*
  %val1 = alloca %String*
  %col2 = alloca %String*
  %col1 = alloca %String*
  %id = alloca i8*
  %url = alloca i8*
  %base_str = alloca i8*
  %num = alloca i32
  store i32 12345, i32* %num
  store i8* getelementptr inbounds ([51 x i8], [51 x i8]* @16, i32 0, i32 0), i8** %base_str
  %base_str1 = load i8*, i8** %base_str
  %0 = call i8* @malloc(i64 16)
  %1 = icmp eq i8* %0, null
  br i1 %1, label %mem_panic, label %mem_ok

mem_panic:                                        ; preds = %entry
  %2 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @21, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok:                                           ; preds = %entry
  %3 = bitcast i8* %0 to %String*
  call void @String__init(%String* %3, i8* %base_str1)
  %4 = call %String* @String_upper(%String* %3)
  %5 = call i8* @String___str(%String* %4)
  %6 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @18, i32 0, i32 0), i8* %5)
  %base_str2 = load i8*, i8** %base_str
  %7 = call i8* @malloc(i64 16)
  %8 = icmp eq i8* %7, null
  br i1 %8, label %mem_panic3, label %mem_ok4

mem_panic3:                                       ; preds = %mem_ok
  %9 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @26, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok4:                                          ; preds = %mem_ok
  %10 = bitcast i8* %7 to %String*
  call void @String__init(%String* %10, i8* %base_str2)
  %11 = call %String* @String_title(%String* %10)
  %12 = call i8* @String___str(%String* %11)
  %13 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @23, i32 0, i32 0), i8* %12)
  %base_str5 = load i8*, i8** %base_str
  %14 = call i8* @malloc(i64 16)
  %15 = icmp eq i8* %14, null
  br i1 %15, label %mem_panic6, label %mem_ok7

mem_panic6:                                       ; preds = %mem_ok4
  %16 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @31, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok7:                                          ; preds = %mem_ok4
  %17 = bitcast i8* %14 to %String*
  call void @String__init(%String* %17, i8* %base_str5)
  %18 = call %String* @String_lower(%String* %17)
  %19 = call i8* @String___str(%String* %18)
  %20 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @28, i32 0, i32 0), i8* %19)
  %base_str8 = load i8*, i8** %base_str
  %21 = call i8* @malloc(i64 16)
  %22 = icmp eq i8* %21, null
  br i1 %22, label %mem_panic9, label %mem_ok10

mem_panic9:                                       ; preds = %mem_ok7
  %23 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @36, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok10:                                         ; preds = %mem_ok7
  %24 = bitcast i8* %21 to %String*
  call void @String__init(%String* %24, i8* %base_str8)
  %25 = call %String* @String_capitalize(%String* %24)
  %26 = call i8* @String___str(%String* %25)
  %27 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @33, i32 0, i32 0), i8* %26)
  %base_str11 = load i8*, i8** %base_str
  %28 = call i8* @malloc(i64 16)
  %29 = icmp eq i8* %28, null
  br i1 %29, label %mem_panic12, label %mem_ok13

mem_panic12:                                      ; preds = %mem_ok10
  %30 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @41, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok13:                                         ; preds = %mem_ok10
  %31 = bitcast i8* %28 to %String*
  call void @String__init(%String* %31, i8* %base_str11)
  %32 = call %String* @String_sentence_case(%String* %31)
  %33 = call i8* @String___str(%String* %32)
  %34 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @38, i32 0, i32 0), i8* %33)
  store i8* getelementptr inbounds ([42 x i8], [42 x i8]* @42, i32 0, i32 0), i8** %url
  %url14 = load i8*, i8** %url
  %35 = call i8* @malloc(i64 16)
  %36 = icmp eq i8* %35, null
  br i1 %36, label %mem_panic15, label %mem_ok16

mem_panic15:                                      ; preds = %mem_ok13
  %37 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @47, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok16:                                         ; preds = %mem_ok13
  %38 = bitcast i8* %35 to %String*
  call void @String__init(%String* %38, i8* %url14)
  %39 = call %String* @String_encode(%String* %38)
  %40 = call i8* @String___str(%String* %39)
  %41 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @44, i32 0, i32 0), i8* %40)
  %base_str17 = load i8*, i8** %base_str
  %42 = call i8* @malloc(i64 16)
  %43 = icmp eq i8* %42, null
  br i1 %43, label %mem_panic18, label %mem_ok19

mem_panic18:                                      ; preds = %mem_ok16
  %44 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @52, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok19:                                         ; preds = %mem_ok16
  %45 = bitcast i8* %42 to %String*
  call void @String__init(%String* %45, i8* %base_str17)
  %46 = call i8* @malloc(i64 16)
  %47 = icmp eq i8* %46, null
  br i1 %47, label %mem_panic20, label %mem_ok21

mem_panic20:                                      ; preds = %mem_ok19
  %48 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @54, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok21:                                         ; preds = %mem_ok19
  %49 = bitcast i8* %46 to %String*
  call void @String__init(%String* %49, i8* getelementptr inbounds ([2 x i8], [2 x i8]* @53, i32 0, i32 0))
  %50 = call i32 @String_count(%String* %45, %String* %49)
  %51 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @48, i32 0, i32 0), i32 %50)
  %base_str22 = load i8*, i8** %base_str
  %52 = call i8* @malloc(i64 16)
  %53 = icmp eq i8* %52, null
  br i1 %53, label %mem_panic23, label %mem_ok24

mem_panic23:                                      ; preds = %mem_ok21
  %54 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @59, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok24:                                         ; preds = %mem_ok21
  %55 = bitcast i8* %52 to %String*
  call void @String__init(%String* %55, i8* %base_str22)
  %56 = call i8* @malloc(i64 16)
  %57 = icmp eq i8* %56, null
  br i1 %57, label %mem_panic25, label %mem_ok26

mem_panic25:                                      ; preds = %mem_ok24
  %58 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @61, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok26:                                         ; preds = %mem_ok24
  %59 = bitcast i8* %56 to %String*
  call void @String__init(%String* %59, i8* getelementptr inbounds ([12 x i8], [12 x i8]* @60, i32 0, i32 0))
  %60 = call i1 @String_endswith(%String* %55, %String* %59)
  %61 = select i1 %60, i8* getelementptr inbounds ([5 x i8], [5 x i8]* @62, i32 0, i32 0), i8* getelementptr inbounds ([6 x i8], [6 x i8]* @63, i32 0, i32 0)
  %62 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @56, i32 0, i32 0), i8* %61)
  %base_str27 = load i8*, i8** %base_str
  %63 = call i8* @malloc(i64 16)
  %64 = icmp eq i8* %63, null
  br i1 %64, label %mem_panic28, label %mem_ok29

mem_panic28:                                      ; preds = %mem_ok26
  %65 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @68, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok29:                                         ; preds = %mem_ok26
  %66 = bitcast i8* %63 to %String*
  call void @String__init(%String* %66, i8* %base_str27)
  %67 = call i8* @malloc(i64 16)
  %68 = icmp eq i8* %67, null
  br i1 %68, label %mem_panic30, label %mem_ok31

mem_panic30:                                      ; preds = %mem_ok29
  %69 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @70, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok31:                                         ; preds = %mem_ok29
  %70 = bitcast i8* %67 to %String*
  call void @String__init(%String* %70, i8* getelementptr inbounds ([9 x i8], [9 x i8]* @69, i32 0, i32 0))
  %71 = call i32 @String_find(%String* %66, %String* %70)
  %72 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @64, i32 0, i32 0), i32 %71)
  %73 = call i8* @malloc(i64 16)
  %74 = icmp eq i8* %73, null
  br i1 %74, label %mem_panic32, label %mem_ok33

mem_panic32:                                      ; preds = %mem_ok31
  %75 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @76, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok33:                                         ; preds = %mem_ok31
  %76 = bitcast i8* %73 to %String*
  call void @String__init(%String* %76, i8* getelementptr inbounds ([26 x i8], [26 x i8]* @75, i32 0, i32 0))
  %77 = call i8* @malloc(i64 16)
  %78 = icmp eq i8* %77, null
  br i1 %78, label %mem_panic34, label %mem_ok35

mem_panic34:                                      ; preds = %mem_ok33
  %79 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([41 x i8], [41 x i8]* @78, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok35:                                         ; preds = %mem_ok33
  %80 = bitcast i8* %77 to %List*
  call void @List__init(%List* %80, i32 1)
  %81 = getelementptr inbounds %List, %List* %80, i32 0, i32 0
  %82 = load i8*, i8** %81
  %83 = bitcast i8* %82 to i8**
  %84 = getelementptr i8*, i8** %83, i32 0
  store i8* getelementptr inbounds ([12 x i8], [12 x i8]* @77, i32 0, i32 0), i8** %84
  %85 = getelementptr inbounds %List, %List* %80, i32 0, i32 1
  store i32 1, i32* %85
  %86 = getelementptr inbounds %List, %List* %80, i32 0, i32 2
  store i32 1, i32* %86
  %87 = call %String* @String_format_list(%String* %76, %List* %80)
  %88 = call i8* @String___str(%String* %87)
  %89 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @72, i32 0, i32 0), i8* %88)
  %90 = call i8* @malloc(i64 16)
  %91 = icmp eq i8* %90, null
  br i1 %91, label %mem_panic36, label %mem_ok37

mem_panic36:                                      ; preds = %mem_ok35
  %92 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @84, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok37:                                         ; preds = %mem_ok35
  %93 = bitcast i8* %90 to %String*
  call void @String__init(%String* %93, i8* getelementptr inbounds ([31 x i8], [31 x i8]* @83, i32 0, i32 0))
  %94 = call i8* @malloc(i64 16)
  %95 = icmp eq i8* %94, null
  br i1 %95, label %mem_panic38, label %mem_ok39

mem_panic38:                                      ; preds = %mem_ok37
  %96 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([41 x i8], [41 x i8]* @89, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok39:                                         ; preds = %mem_ok37
  %97 = bitcast i8* %94 to %List*
  call void @List__init(%List* %97, i32 2)
  %98 = getelementptr inbounds %List, %List* %97, i32 0, i32 0
  %99 = load i8*, i8** %98
  %100 = bitcast i8* %99 to i8**
  %101 = getelementptr i8*, i8** %100, i32 0
  store i8* getelementptr inbounds ([5 x i8], [5 x i8]* @85, i32 0, i32 0), i8** %101
  %102 = getelementptr i8*, i8** %100, i32 1
  store i8* getelementptr inbounds ([4 x i8], [4 x i8]* @87, i32 0, i32 0), i8** %102
  %103 = getelementptr inbounds %List, %List* %97, i32 0, i32 1
  store i32 2, i32* %103
  %104 = getelementptr inbounds %List, %List* %97, i32 0, i32 2
  store i32 2, i32* %104
  %105 = call i8* @malloc(i64 16)
  %106 = icmp eq i8* %105, null
  br i1 %106, label %mem_panic40, label %mem_ok41

mem_panic40:                                      ; preds = %mem_ok39
  %107 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([41 x i8], [41 x i8]* @90, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok41:                                         ; preds = %mem_ok39
  %108 = bitcast i8* %105 to %List*
  call void @List__init(%List* %108, i32 2)
  %109 = getelementptr inbounds %List, %List* %108, i32 0, i32 0
  %110 = load i8*, i8** %109
  %111 = bitcast i8* %110 to i8**
  %112 = getelementptr i8*, i8** %111, i32 0
  store i8* getelementptr inbounds ([8 x i8], [8 x i8]* @86, i32 0, i32 0), i8** %112
  %113 = getelementptr i8*, i8** %111, i32 1
  store i8* getelementptr inbounds ([12 x i8], [12 x i8]* @88, i32 0, i32 0), i8** %113
  %114 = getelementptr inbounds %List, %List* %108, i32 0, i32 1
  store i32 2, i32* %114
  %115 = getelementptr inbounds %List, %List* %108, i32 0, i32 2
  store i32 2, i32* %115
  %116 = call %String* @String_format_map(%String* %93, %List* %97, %List* %108)
  %117 = call i8* @String___str(%String* %116)
  %118 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @80, i32 0, i32 0), i8* %117)
  %base_str42 = load i8*, i8** %base_str
  %119 = call i8* @malloc(i64 16)
  %120 = icmp eq i8* %119, null
  br i1 %120, label %mem_panic43, label %mem_ok44

mem_panic43:                                      ; preds = %mem_ok41
  %121 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @95, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok44:                                         ; preds = %mem_ok41
  %122 = bitcast i8* %119 to %String*
  call void @String__init(%String* %122, i8* %base_str42)
  %123 = call i8* @malloc(i64 16)
  %124 = icmp eq i8* %123, null
  br i1 %124, label %mem_panic45, label %mem_ok46

mem_panic45:                                      ; preds = %mem_ok44
  %125 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @97, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok46:                                         ; preds = %mem_ok44
  %126 = bitcast i8* %123 to %String*
  call void @String__init(%String* %126, i8* getelementptr inbounds ([7 x i8], [7 x i8]* @96, i32 0, i32 0))
  %127 = call i32 @String_index(%String* %122, %String* %126)
  %128 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @91, i32 0, i32 0), i32 %127)
  %base_str47 = load i8*, i8** %base_str
  %129 = call i8* @malloc(i64 16)
  %130 = icmp eq i8* %129, null
  br i1 %130, label %mem_panic48, label %mem_ok49

mem_panic48:                                      ; preds = %mem_ok46
  %131 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @102, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok49:                                         ; preds = %mem_ok46
  %132 = bitcast i8* %129 to %String*
  call void @String__init(%String* %132, i8* %base_str47)
  %133 = call %String* @String_trim(%String* %132)
  %134 = call i8* @String___str(%String* %133)
  %135 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @99, i32 0, i32 0), i8* %134)
  %base_str50 = load i8*, i8** %base_str
  %136 = call i8* @malloc(i64 16)
  %137 = icmp eq i8* %136, null
  br i1 %137, label %mem_panic51, label %mem_ok52

mem_panic51:                                      ; preds = %mem_ok49
  %138 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @107, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok52:                                         ; preds = %mem_ok49
  %139 = bitcast i8* %136 to %String*
  call void @String__init(%String* %139, i8* %base_str50)
  %140 = call i8* @malloc(i64 16)
  %141 = icmp eq i8* %140, null
  br i1 %141, label %mem_panic53, label %mem_ok54

mem_panic53:                                      ; preds = %mem_ok52
  %142 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @109, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok54:                                         ; preds = %mem_ok52
  %143 = bitcast i8* %140 to %String*
  call void @String__init(%String* %143, i8* getelementptr inbounds ([7 x i8], [7 x i8]* @108, i32 0, i32 0))
  %144 = call i8* @malloc(i64 16)
  %145 = icmp eq i8* %144, null
  br i1 %145, label %mem_panic55, label %mem_ok56

mem_panic55:                                      ; preds = %mem_ok54
  %146 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @111, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok56:                                         ; preds = %mem_ok54
  %147 = bitcast i8* %144 to %String*
  call void @String__init(%String* %147, i8* getelementptr inbounds ([7 x i8], [7 x i8]* @110, i32 0, i32 0))
  %148 = call %String* @String_replace(%String* %139, %String* %143, %String* %147)
  %149 = call i8* @String___str(%String* %148)
  %150 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @104, i32 0, i32 0), i8* %149)
  %base_str57 = load i8*, i8** %base_str
  %151 = call i8* @malloc(i64 16)
  %152 = icmp eq i8* %151, null
  br i1 %152, label %mem_panic58, label %mem_ok59

mem_panic58:                                      ; preds = %mem_ok56
  %153 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @116, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok59:                                         ; preds = %mem_ok56
  %154 = bitcast i8* %151 to %String*
  call void @String__init(%String* %154, i8* %base_str57)
  %155 = call i8* @malloc(i64 16)
  %156 = icmp eq i8* %155, null
  br i1 %156, label %mem_panic60, label %mem_ok61

mem_panic60:                                      ; preds = %mem_ok59
  %157 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @118, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok61:                                         ; preds = %mem_ok59
  %158 = bitcast i8* %155 to %String*
  call void @String__init(%String* %158, i8* getelementptr inbounds ([7 x i8], [7 x i8]* @117, i32 0, i32 0))
  %159 = call i1 @String_startswith(%String* %154, %String* %158)
  %160 = select i1 %159, i8* getelementptr inbounds ([5 x i8], [5 x i8]* @119, i32 0, i32 0), i8* getelementptr inbounds ([6 x i8], [6 x i8]* @120, i32 0, i32 0)
  %161 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @113, i32 0, i32 0), i8* %160)
  %base_str62 = load i8*, i8** %base_str
  %162 = call i8* @malloc(i64 16)
  %163 = icmp eq i8* %162, null
  br i1 %163, label %mem_panic63, label %mem_ok64

mem_panic63:                                      ; preds = %mem_ok61
  %164 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @125, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok64:                                         ; preds = %mem_ok61
  %165 = bitcast i8* %162 to %String*
  call void @String__init(%String* %165, i8* %base_str62)
  %166 = call %String* @String_substring(%String* %165, i32 5, i32 10)
  %167 = call i8* @String___str(%String* %166)
  %168 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @122, i32 0, i32 0), i8* %167)
  %169 = call i8* @malloc(i64 16)
  %170 = icmp eq i8* %169, null
  br i1 %170, label %mem_panic65, label %mem_ok66

mem_panic65:                                      ; preds = %mem_ok64
  %171 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @131, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok66:                                         ; preds = %mem_ok64
  %172 = bitcast i8* %169 to %String*
  call void @String__init(%String* %172, i8* getelementptr inbounds ([2 x i8], [2 x i8]* @130, i32 0, i32 0))
  %173 = call i8* @malloc(i64 16)
  %174 = icmp eq i8* %173, null
  br i1 %174, label %mem_panic67, label %mem_ok68

mem_panic67:                                      ; preds = %mem_ok66
  %175 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([41 x i8], [41 x i8]* @132, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok68:                                         ; preds = %mem_ok66
  %176 = bitcast i8* %173 to %List*
  call void @List__init(%List* %176, i32 3)
  %177 = getelementptr inbounds %List, %List* %176, i32 0, i32 0
  %178 = load i8*, i8** %177
  %179 = bitcast i8* %178 to i8**
  %180 = getelementptr i8*, i8** %179, i32 0
  store i8* inttoptr (i32 1 to i8*), i8** %180
  %181 = getelementptr i8*, i8** %179, i32 1
  store i8* inttoptr (i32 2 to i8*), i8** %181
  %182 = getelementptr i8*, i8** %179, i32 2
  store i8* inttoptr (i32 3 to i8*), i8** %182
  %183 = getelementptr inbounds %List, %List* %176, i32 0, i32 1
  store i32 3, i32* %183
  %184 = getelementptr inbounds %List, %List* %176, i32 0, i32 2
  store i32 3, i32* %184
  %185 = call %String* @String_join(%String* %172, %List* %176)
  %186 = call i8* @String___str(%String* %185)
  %187 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @127, i32 0, i32 0), i8* %186)
  %188 = call i8* @malloc(i64 16)
  %189 = icmp eq i8* %188, null
  br i1 %189, label %mem_panic69, label %mem_ok70

mem_panic69:                                      ; preds = %mem_ok68
  %190 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @138, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok70:                                         ; preds = %mem_ok68
  %191 = bitcast i8* %188 to %String*
  call void @String__init(%String* %191, i8* getelementptr inbounds ([3 x i8], [3 x i8]* @137, i32 0, i32 0))
  %192 = call i1 @String_isdigit(%String* %191)
  %193 = select i1 %192, i8* getelementptr inbounds ([5 x i8], [5 x i8]* @139, i32 0, i32 0), i8* getelementptr inbounds ([6 x i8], [6 x i8]* @140, i32 0, i32 0)
  %194 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @134, i32 0, i32 0), i8* %193)
  %195 = call i8* @malloc(i64 16)
  %196 = icmp eq i8* %195, null
  br i1 %196, label %mem_panic71, label %mem_ok72

mem_panic71:                                      ; preds = %mem_ok70
  %197 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @146, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok72:                                         ; preds = %mem_ok70
  %198 = bitcast i8* %195 to %String*
  call void @String__init(%String* %198, i8* getelementptr inbounds ([3 x i8], [3 x i8]* @145, i32 0, i32 0))
  %199 = call i1 @String_isalpha(%String* %198)
  %200 = select i1 %199, i8* getelementptr inbounds ([5 x i8], [5 x i8]* @147, i32 0, i32 0), i8* getelementptr inbounds ([6 x i8], [6 x i8]* @148, i32 0, i32 0)
  %201 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @142, i32 0, i32 0), i8* %200)
  %base_str73 = load i8*, i8** %base_str
  %202 = call i8* @malloc(i64 16)
  %203 = icmp eq i8* %202, null
  br i1 %203, label %mem_panic74, label %mem_ok75

mem_panic74:                                      ; preds = %mem_ok72
  %204 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @153, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok75:                                         ; preds = %mem_ok72
  %205 = bitcast i8* %202 to %String*
  call void @String__init(%String* %205, i8* %base_str73)
  %206 = call i8* @malloc(i64 16)
  %207 = icmp eq i8* %206, null
  br i1 %207, label %mem_panic76, label %mem_ok77

mem_panic76:                                      ; preds = %mem_ok75
  %208 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @155, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok77:                                         ; preds = %mem_ok75
  %209 = bitcast i8* %206 to %String*
  call void @String__init(%String* %209, i8* getelementptr inbounds ([2 x i8], [2 x i8]* @154, i32 0, i32 0))
  %210 = call %String* @String_remove(%String* %205, %String* %209)
  %211 = call i8* @String___str(%String* %210)
  %212 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @150, i32 0, i32 0), i8* %211)
  %213 = call i8* @malloc(i64 16)
  %214 = icmp eq i8* %213, null
  br i1 %214, label %mem_panic78, label %mem_ok79

mem_panic78:                                      ; preds = %mem_ok77
  %215 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @161, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok79:                                         ; preds = %mem_ok77
  %216 = bitcast i8* %213 to %String*
  call void @String__init(%String* %216, i8* getelementptr inbounds ([2 x i8], [2 x i8]* @160, i32 0, i32 0))
  %217 = call %String* @String_repeat(%String* %216, i32 10)
  %218 = call i8* @String___str(%String* %217)
  %219 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @157, i32 0, i32 0), i8* %218)
  %220 = call i8* @malloc(i64 16)
  %221 = icmp eq i8* %220, null
  br i1 %221, label %mem_panic80, label %mem_ok81

mem_panic80:                                      ; preds = %mem_ok79
  %222 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @167, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok81:                                         ; preds = %mem_ok79
  %223 = bitcast i8* %220 to %String*
  call void @String__init(%String* %223, i8* getelementptr inbounds ([9 x i8], [9 x i8]* @166, i32 0, i32 0))
  %224 = call %String* @String_reverse(%String* %223)
  %225 = call i8* @String___str(%String* %224)
  %226 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @163, i32 0, i32 0), i8* %225)
  store i8* getelementptr inbounds ([3 x i8], [3 x i8]* @168, i32 0, i32 0), i8** %id
  %id82 = load i8*, i8** %id
  %227 = call i8* @malloc(i64 16)
  %228 = icmp eq i8* %227, null
  br i1 %228, label %mem_panic83, label %mem_ok84

mem_panic83:                                      ; preds = %mem_ok81
  %229 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @174, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok84:                                         ; preds = %mem_ok81
  %230 = bitcast i8* %227 to %String*
  call void @String__init(%String* %230, i8* %id82)
  %231 = call %String* @String_zfill(%String* %230, i32 5)
  %232 = call i8* @malloc(i64 16)
  %233 = icmp eq i8* %232, null
  br i1 %233, label %mem_panic85, label %mem_ok86

mem_panic85:                                      ; preds = %mem_ok84
  %234 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @175, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok86:                                         ; preds = %mem_ok84
  %235 = bitcast i8* %232 to %String*
  call void @String__init(%String* %235, i8* getelementptr inbounds ([5 x i8], [5 x i8]* @173, i32 0, i32 0))
  %236 = call %String* @String___add(%String* %235, %String* %231)
  %237 = call i8* @String___str(%String* %236)
  %238 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @170, i32 0, i32 0), i8* %237)
  %239 = call i8* @malloc(i64 16)
  %240 = icmp eq i8* %239, null
  br i1 %240, label %mem_panic87, label %mem_ok88

mem_panic87:                                      ; preds = %mem_ok86
  %241 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @177, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok88:                                         ; preds = %mem_ok86
  %242 = bitcast i8* %239 to %String*
  call void @String__init(%String* %242, i8* getelementptr inbounds ([5 x i8], [5 x i8]* @176, i32 0, i32 0))
  %243 = call i8* @malloc(i64 16)
  %244 = icmp eq i8* %243, null
  br i1 %244, label %mem_panic89, label %mem_ok90

mem_panic89:                                      ; preds = %mem_ok88
  %245 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @179, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok90:                                         ; preds = %mem_ok88
  %246 = bitcast i8* %243 to %String*
  call void @String__init(%String* %246, i8* getelementptr inbounds ([2 x i8], [2 x i8]* @178, i32 0, i32 0))
  %247 = call %String* @String_ljust(%String* %242, i32 10, %String* %246)
  store %String* %247, %String** %col1
  %248 = call i8* @malloc(i64 16)
  %249 = icmp eq i8* %248, null
  br i1 %249, label %mem_panic91, label %mem_ok92

mem_panic91:                                      ; preds = %mem_ok90
  %250 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @181, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok92:                                         ; preds = %mem_ok90
  %251 = bitcast i8* %248 to %String*
  call void @String__init(%String* %251, i8* getelementptr inbounds ([6 x i8], [6 x i8]* @180, i32 0, i32 0))
  %252 = call i8* @malloc(i64 16)
  %253 = icmp eq i8* %252, null
  br i1 %253, label %mem_panic93, label %mem_ok94

mem_panic93:                                      ; preds = %mem_ok92
  %254 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @183, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok94:                                         ; preds = %mem_ok92
  %255 = bitcast i8* %252 to %String*
  call void @String__init(%String* %255, i8* getelementptr inbounds ([2 x i8], [2 x i8]* @182, i32 0, i32 0))
  %256 = call %String* @String_rjust(%String* %251, i32 8, %String* %255)
  store %String* %256, %String** %col2
  %col195 = load %String*, %String** %col1
  %257 = call i8* @malloc(i64 16)
  %258 = icmp eq i8* %257, null
  br i1 %258, label %mem_panic96, label %mem_ok97

mem_panic96:                                      ; preds = %mem_ok94
  %259 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @189, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok97:                                         ; preds = %mem_ok94
  %260 = bitcast i8* %257 to %String*
  call void @String__init(%String* %260, i8* getelementptr inbounds ([2 x i8], [2 x i8]* @188, i32 0, i32 0))
  %261 = call %String* @String___add(%String* %col195, %String* %260)
  %col298 = load %String*, %String** %col2
  %262 = call %String* @String___add(%String* %261, %String* %col298)
  %263 = call i8* @String___str(%String* %262)
  %264 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @185, i32 0, i32 0), i8* %263)
  %265 = call i8* @malloc(i64 16)
  %266 = icmp eq i8* %265, null
  br i1 %266, label %mem_panic99, label %mem_ok100

mem_panic99:                                      ; preds = %mem_ok97
  %267 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @191, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok100:                                        ; preds = %mem_ok97
  %268 = bitcast i8* %265 to %String*
  call void @String__init(%String* %268, i8* getelementptr inbounds ([6 x i8], [6 x i8]* @190, i32 0, i32 0))
  %269 = call i8* @malloc(i64 16)
  %270 = icmp eq i8* %269, null
  br i1 %270, label %mem_panic101, label %mem_ok102

mem_panic101:                                     ; preds = %mem_ok100
  %271 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @193, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok102:                                        ; preds = %mem_ok100
  %272 = bitcast i8* %269 to %String*
  call void @String__init(%String* %272, i8* getelementptr inbounds ([2 x i8], [2 x i8]* @192, i32 0, i32 0))
  %273 = call %String* @String_ljust(%String* %268, i32 10, %String* %272)
  store %String* %273, %String** %val1
  %274 = call i8* @malloc(i64 16)
  %275 = icmp eq i8* %274, null
  br i1 %275, label %mem_panic103, label %mem_ok104

mem_panic103:                                     ; preds = %mem_ok102
  %276 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @195, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok104:                                        ; preds = %mem_ok102
  %277 = bitcast i8* %274 to %String*
  call void @String__init(%String* %277, i8* getelementptr inbounds ([6 x i8], [6 x i8]* @194, i32 0, i32 0))
  %278 = call i8* @malloc(i64 16)
  %279 = icmp eq i8* %278, null
  br i1 %279, label %mem_panic105, label %mem_ok106

mem_panic105:                                     ; preds = %mem_ok104
  %280 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @197, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok106:                                        ; preds = %mem_ok104
  %281 = bitcast i8* %278 to %String*
  call void @String__init(%String* %281, i8* getelementptr inbounds ([2 x i8], [2 x i8]* @196, i32 0, i32 0))
  %282 = call %String* @String_rjust(%String* %277, i32 8, %String* %281)
  store %String* %282, %String** %val2
  %val1107 = load %String*, %String** %val1
  %283 = call i8* @malloc(i64 16)
  %284 = icmp eq i8* %283, null
  br i1 %284, label %mem_panic108, label %mem_ok109

mem_panic108:                                     ; preds = %mem_ok106
  %285 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @203, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok109:                                        ; preds = %mem_ok106
  %286 = bitcast i8* %283 to %String*
  call void @String__init(%String* %286, i8* getelementptr inbounds ([2 x i8], [2 x i8]* @202, i32 0, i32 0))
  %287 = call %String* @String___add(%String* %val1107, %String* %286)
  %val2110 = load %String*, %String** %val2
  %288 = call %String* @String___add(%String* %287, %String* %val2110)
  %289 = call i8* @String___str(%String* %288)
  %290 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @199, i32 0, i32 0), i8* %289)
  %291 = call i8* @malloc(i64 16)
  %292 = icmp eq i8* %291, null
  br i1 %292, label %mem_panic111, label %mem_ok112

then:                                             ; preds = %mem_ok114
  %293 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @209, i32 0, i32 0), i8* getelementptr inbounds ([10 x i8], [10 x i8]* @212, i32 0, i32 0))
  br label %ifcont

mem_panic111:                                     ; preds = %mem_ok109
  %294 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @205, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok112:                                        ; preds = %mem_ok109
  %295 = bitcast i8* %291 to %String*
  call void @String__init(%String* %295, i8* getelementptr inbounds ([5 x i8], [5 x i8]* @204, i32 0, i32 0))
  %296 = call i8* @malloc(i64 16)
  %297 = icmp eq i8* %296, null
  br i1 %297, label %mem_panic113, label %mem_ok114

mem_panic113:                                     ; preds = %mem_ok112
  %298 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([43 x i8], [43 x i8]* @207, i32 0, i32 0))
  call void @exit(i32 1)
  unreachable

mem_ok114:                                        ; preds = %mem_ok112
  %299 = bitcast i8* %296 to %String*
  call void @String__init(%String* %299, i8* getelementptr inbounds ([3 x i8], [3 x i8]* @206, i32 0, i32 0))
  %300 = call i1 @String_contains(%String* %295, %String* %299)
  br i1 %300, label %then, label %ifcont

ifcont:                                           ; preds = %then, %mem_ok114
  ret i32 0
}
