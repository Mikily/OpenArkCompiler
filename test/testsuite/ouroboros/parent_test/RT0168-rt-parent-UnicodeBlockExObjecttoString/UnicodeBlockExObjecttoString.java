/*
 * Copyright (c) [2020] Huawei Technologies Co.,Ltd.All rights reserved.
 *
 * OpenArkCompiler is licensed under the Mulan PSL v1.
 * You can use this software according to the terms and conditions of the Mulan PSL v1.
 * You may obtain a copy of Mulan PSL v1 at:
 *
 *     http://license.coscl.org.cn/MulanPSL
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR
 * FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v1 for more details.
 * -@TestCaseID: UnicodeBlockExObjecttoString.java
 * -@TestCaseName: Exception in Character/UnicodeBlock: final String toString()
 * -@TestCaseType: Function Test
 * -@RequirementName: 补充重写类的父类方法
 * -@Brief:Test UnicodeBlock api toString extends from Object
 * -@Expect:0\n
 * -@Priority: High
 * -@Source: UnicodeBlockExObjecttoString.java
 * -@ExecuteClass: UnicodeBlockExObjecttoString
 * -@ExecuteArgs:
 */

import java.lang.Character;

public class UnicodeBlockExObjecttoString {
    static int res = 99;

    public static void main(String argv[]) {
        System.out.println(new UnicodeBlockExObjecttoString().run());
    }

    /**
     * main test fun
     * @return status code
     */
    public int run() {
        int result = 2; /*STATUS_FAILED*/
        try {
            result = unicodeBlockExObjecttoString1();
        } catch (Exception e) {
            UnicodeBlockExObjecttoString.res = UnicodeBlockExObjecttoString.res - 20;
        }

        if (result == 4 && UnicodeBlockExObjecttoString.res == 89) {
            result = 0;
        }

        return result;
    }

    private int unicodeBlockExObjecttoString1() {
        int result1 = 4; /*STATUS_FAILED*/
        // final String toString()
        Character.UnicodeBlock unB1 = null;
        for (int cp = 0; cp <= 10; ++cp) {
            unB1 = Character.UnicodeBlock.of(cp);
        }

        String px1 = unB1.toString();
        if (px1.equals("BASIC_LATIN")) {
            UnicodeBlockExObjecttoString.res = UnicodeBlockExObjecttoString.res - 10;
        }
        return result1;
    }
}
// EXEC:%maple  %f %build_option -o %n.so
// EXEC:%run %n.so %n %run_option | compare %f
// ASSERT: scan 0\n