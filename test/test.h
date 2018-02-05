//
//  test.h
//  Residue
//
//  Copyright 2017-present Muflihun Labs
//
//  Author: @abumusamq
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//

#ifndef TEST_H
#define TEST_H

#include "testx.h"
#include <easylogging++.h>
#include <gtest/gtest.h>

#define PARAM(v) item.get<v>()

using namespace muflihun::testx;

#ifdef RESIDUE_ON_CI
// CI IS ON UTC
unsigned long offsetTimezone = 11
#else
unsigned long offsetTimezone = 0;
#endif

#endif // TEST_H
