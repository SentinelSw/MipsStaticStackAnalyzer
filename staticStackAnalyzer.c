/**
 * @file
 * @brief   MIPS32 static stack analyzer
 * @author  Florian Kaup, i.A. Magnetic Sense GmbH (kaup.florian@sentinelsw.de)
 * @date    2020
 * @copyright   GPLv3 (https://www.gnu.org/licenses/gpl-3.0)
 * @details
 *
 * staticStackAnalyzer
 * -------------------
 *
 * This program parses an ELF file and calculates the estimated stack usage of it.
 * It is build for ELF files compiled by mips gcc in general and xc32 from
 * Microchip Technology Inc. in special with MIPS32 release 5 target architecture.
 * (This should apply to whole PIC32MZ-family, maybe even more)
 *
 * After parsing the ELF file, this function prints a markdown compatible table
 * of the results, sorted and limited for your needs. This can be directly
 * piped into a *.md file, so doxygen can include it into your program documentation.
 *
 * @bug This program cannot handle recursive function calls and will hang attempting to resolve one.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>


/**
 * Linked List Entry for function information.
 * This struct is used for collecting information about functions.
 * Each function has its own struct, all are linked together
 * as a linked list.
 */
typedef struct functionInfo
{
    char * name;                ///< Name of the function
    uint32_t start;             ///< Start address of the function
    uint32_t end;               ///< End address of the function
    uint32_t ownStack;          ///< Estimated maximum stack usage
    uint32_t deepest;           ///< Estimated amount of stack bytes for this function and the deepest call tree
    uint32_t * jumpsTo;         ///< Array of addresses, where the function jumps and branches to
    uint32_t numJumpsTo;        ///< The number of entries in ::jumpsTo
    bool usesFunctionPointers;  ///< Flag for potential function pointer usage
    bool isProcessed;           ///< Flag if deepest stack usage is calculated
    struct functionInfo * next; ///< Pointer to the next list member
} functionInfo_t;

/**
 * Pointer to first element.
 */
functionInfo_t * firstFunctionInfo;

/**
 * Wait for time.
 * This function hangs the program for a desired amount of time.
 * @param milliseconds  The time in milliseconds to wait.
 */
void busywait (uint32_t milliseconds)
{
    uint32_t start = clock();
    while ( (clock()-start) * (1000 / CLOCKS_PER_SEC) < milliseconds);
}

/**
 * Create handle for reading disassembly.
 * This function calls xc32 objdump for disassembling the passed elf file.
 * Additionally it gives objdump a head start, so buffer underrun is prevented.
 * @note If you are using a different xc32 version or have a different install location, adapt the path in this function!
 * @param filename  The file to disassemble
 * @return  The file handle for reading the disassembly
 */
FILE* openDisassembly (char * filename)
{
    char command[500];
    sprintf(command, "\"C:\\Program Files (x86)\\Microchip\\xc32\\v2.30\\bin\\xc32-objdump.exe\" -d %s", filename);
    FILE* commandHandle = popen(command, "r");
    busywait(100);
    return commandHandle;
}

/**
 * Skip to next text section.
 * Call this function for skipping over sections, which are not ".text".
 * This is useful for skipping .rodata, .vectors and anything else with
 * no interest. The file handle is moved to the next line after the magic
 * pattern, or EOF if not found.
 * @param disassemblyInput  The file handle to use
 */
void findNextTextSection (FILE* disassemblyInput)
{
    static const char * expectedString = "Disassembly of section .text";
    char inputBuffer[500];
    fgets(inputBuffer, sizeof(inputBuffer), disassemblyInput);

    while (
            (feof(disassemblyInput) == 0) &&
            (strncmp(inputBuffer, expectedString, strlen(expectedString)) != 0)
    )     fgets(inputBuffer, sizeof(inputBuffer), disassemblyInput);

    // jump over and ignore newline after expected string
    fgets(inputBuffer, sizeof(inputBuffer), disassemblyInput);
}

/**
 * Cleaning jumpTo table for function info.
 * The table containing jump and branch targets is cleaned and trimmed
 * by this function. All jump targets, which points into own address range
 * are discarded (hence also recursive calls), and memory is trimmed to
 * fit the final amount of jumps.
 * @param functionInfo The function info struct to clean up
 */
void cleanupFunctionInfo (functionInfo_t* functionInfo)
{
    uint32_t jumpsTo[1000];
    uint32_t numJumpsTo = 0;

    for (uint32_t i = 0; i<functionInfo->numJumpsTo; i++)
    {
        if (
                (functionInfo->jumpsTo[i] > functionInfo->end) ||
                (functionInfo->jumpsTo[i] < functionInfo->start)
                )
        {
            jumpsTo[numJumpsTo] = functionInfo->jumpsTo[i];
            numJumpsTo++;
        }
    }
    free(functionInfo->jumpsTo);
    functionInfo->jumpsTo = malloc(numJumpsTo * sizeof(uint32_t));
    memcpy(functionInfo->jumpsTo, jumpsTo, numJumpsTo * sizeof(uint32_t));
    functionInfo->numJumpsTo = numJumpsTo;
}

/**
 * Create a new function info list element.
 * A function info list element is allocated and filled with
 * information from disassembly label informatio0n.
 * @param disassemblyInput The disassembly label to parse
 * @return Pointer to a function info, filled with name and start address
 */
functionInfo_t* createNewFunctionInfo (char * label)
{
    uint32_t address = strtoul(label, NULL, 16);
    char * name = strchr(label, '<');
    char * nameEnd = strchr(label, '>');
    if ( (name == 0) || (nameEnd == 0) )
    {
        return 0;
    }

    functionInfo_t* newOne = malloc(sizeof(functionInfo_t));
    newOne->name = malloc((nameEnd-name)*sizeof(char));
    memcpy(newOne->name, name+1, (nameEnd-name)-1);
    newOne->name[(nameEnd-name)-1] = 0;
    newOne->start = address;
    newOne->end = address;
    newOne->deepest = 0;
    newOne->ownStack = 0;
    newOne->jumpsTo = malloc(10000*sizeof(uint32_t));
    newOne->numJumpsTo = 0;
    newOne->usesFunctionPointers = false;
    newOne->isProcessed = false;
    newOne->next = 0;

    return newOne;
}

/**
 * Find function information by memory address.
 * This function searches the linked list for memory address.
 * The given address is checked, if it matches the address
 * range of function information.
 * @param address The address to find in function information
 * @return The function information containing the requested address
 */
functionInfo_t * findFunctionByAddress (uint32_t address)
{
    functionInfo_t * current = firstFunctionInfo;
    while (current)
    {
        if ( (current->start <= address) && (current->end >= address) )
        {
            break;
        }
        current = current->next;
    }
    return current;
}

/**
 * Calculate and return deepest stack usage.
 * If the deepest stack usage is not yet calculated, it is calculated
 * synchronously. Else the precalculated stack usage is returned.
 * This function is used to update all stack information elements
 * with their deepest stack usage. This function is recursive.
 * @param target The stack information for which the stack usage has to be calculated and returned
 * @return The deepest stack usage of the given stack information
 */
uint32_t getDeepestStackUsage (functionInfo_t * target)
{
    if (target->isProcessed == false)
    {
        // avoid recursive call to this function information
        target->isProcessed = true;
        for (uint32_t i = 0; i<target->numJumpsTo; i++)
        {
            functionInfo_t * jumpTarget = findFunctionByAddress(target->jumpsTo[i]);
            if (jumpTarget == 0)
            {
                printf("Error: Jump Target not found! Function %s jumps to 0x%x\n", target->name, target->jumpsTo[i]);
                continue;
            }
            uint32_t thisBranch = getDeepestStackUsage(jumpTarget);
            if (thisBranch > target->deepest)
            {
                target->deepest = thisBranch;
            }
        }
        target->deepest += target->ownStack;
    }

    return target->deepest;
}

/**
 * Sorts stack information linked list for deepest stack usage.
 * Descending. List pointed by ::firstStackInfo.
 */
void sortForDeepest (void)
{
    functionInfo_t * current = firstFunctionInfo;
    uint32_t count = 0;
    while (current)
    {
        count ++;
        current = current->next;
    }

    for (uint32_t i = count-1; i > 0; i--)
    {
        if (firstFunctionInfo->deepest < firstFunctionInfo->next->deepest)
        {
            functionInfo_t * swap = firstFunctionInfo;
            firstFunctionInfo = firstFunctionInfo->next;
            swap->next = firstFunctionInfo->next;
            firstFunctionInfo->next = swap;

        }
        current = firstFunctionInfo;
        for (uint32_t j = 1; j < i; j++)
        {
            if (current->next->deepest < current->next->next->deepest)
            {
                functionInfo_t * swap = current->next;
                current->next = current->next->next;
                swap->next = current->next->next;
                current->next->next = swap;

            }
            current = current->next;
        }
    }
}

/**
 * Sorts stack information linked list for own stack usage.
 * Descending. List pointed by ::firstStackInfo.
 */
void sortForOwn (void)
{
    functionInfo_t * current = firstFunctionInfo;
    uint32_t count = 0;
    while (current)
    {
        count ++;
        current = current->next;
    }

    for (uint32_t i = count-1; i > 0; i--)
    {
        if (firstFunctionInfo->ownStack < firstFunctionInfo->next->ownStack)
        {
            functionInfo_t * swap = firstFunctionInfo;
            firstFunctionInfo = firstFunctionInfo->next;
            swap->next = firstFunctionInfo->next;
            firstFunctionInfo->next = swap;

        }
        current = firstFunctionInfo;
        for (uint32_t j = 1; j < i; j++)
        {
            if (current->next->ownStack < current->next->next->ownStack)
            {
                functionInfo_t * swap = current->next;
                current->next = current->next->next;
                swap->next = current->next->next;
                current->next->next = swap;

            }
            current = current->next;
        }
    }
}

/**
 * Print stack information.
 * The first elements of stack information linked list are printed
 * to console. It is formated as a markdown compliant table, so you
 * can easily import it into doxygen or anything else documentary
 * related.
 * @param num The number of elements to print
 */
void printStackInfo (uint32_t num)
{
    printf("\n|%-50s|%-15s|%-15s|%-15s|\n", "Name", "Own", "Deepest", "Indirect Calls");
    printf("|--------------------------------------------------|---------------|---------------|---------------|\n");
    functionInfo_t * current = firstFunctionInfo;
    for (uint32_t i = 0; i<num && current; i++)
    {
        printf("|%-50s|%-15i|%-15i|%-15c|\n", current->name, current->ownStack, current->deepest, current->usesFunctionPointers ? '*' : ' ');
        current = current->next;
    }
}

/**
 * Main entry point.
 * Should I say more?
 * @param argc  number of arguments in argv
 * @param argv  arguments from command line
 * @return always 0
 */
int main (int argc, char** argv)
{
    char sorting = 'd';
    uint32_t printcount = 10;
    char * filename = 0;
    for (int i=1; i<argc; i++)
    {
        if (0 == strncmp("-s",argv[i], 2))
        {
            sorting = argv[i][2];
        }
        else if (0 == strncmp("-n",argv[i], 2))
        {
            printcount = strtol(argv[i]+2, 0, 0);
        }
        else
        {
            filename = argv[i];
        }
    }

    if (filename == 0)
    {
        printf("Usage: %s [-s<sorting>] [-n<number>] <input file>\n"
                "Options:\n"
                "  -s<sorting>   Sorting of results, d=Deepest o=Own\n"
                "  -n<number>    The number of entries printed, -1 for all\n"
                "  <input file>  The ELF file to parse\n"
                "\n"
                "Report is printed as markdown table.\n"
                "Content:\n"
                "  Name:           The name of the function as the label in ELF file states.\n"
                "  Own:            The stack usage of this function by itself.\n"
                "  Deepest:        The maximum stack usage of this function and all called function.\n"
                "  Indirect Calls: This function uses function pointers, so the deepest stack usage cannot be determined.\n"
                "\n",
                argv[0]);
        return -1;
    }

    //-----------------------------------------------------------------------------------
    // parse elf disassembly and gather stack and calltree information

    FILE* disassemblyInput = openDisassembly(filename);

    {
        findNextTextSection(disassemblyInput);
        char inputBuffer[500];
        fgets(inputBuffer, sizeof(inputBuffer), disassemblyInput);
        firstFunctionInfo = createNewFunctionInfo(inputBuffer);
    }

    functionInfo_t* currentFunctionInfo = firstFunctionInfo;

    while(feof(disassemblyInput) == 0)
    {
        if (currentFunctionInfo == 0)
        {
            printf("something went wrong...\n");
            return -1;
        }

        char inputBuffer[500];
        fgets(inputBuffer, sizeof(inputBuffer), disassemblyInput);

        // a new section begins
        static const char * sectionString = "Disassembly of section ";
        if ( 0 != strstr(inputBuffer, sectionString) )
        {
            cleanupFunctionInfo(currentFunctionInfo);
            static const char * textSectionString = "Disassembly of section .text";
            if ( 0 == strstr(inputBuffer, textSectionString) )
            {
                findNextTextSection(disassemblyInput);
            }
            fgets(inputBuffer, sizeof(inputBuffer), disassemblyInput);
            currentFunctionInfo->next = createNewFunctionInfo(inputBuffer);
            currentFunctionInfo = currentFunctionInfo->next;
            continue;
        }

        // a new label was found
        static const char * labelEndString = ">:";
        if ( 0 != strstr(inputBuffer, labelEndString) )
        {
            char * labelName = strchr(inputBuffer, '<') + 1;
            // ignore internal labels
            if (labelName[0] != '.')
            {
                cleanupFunctionInfo(currentFunctionInfo);
                currentFunctionInfo->next = createNewFunctionInfo(inputBuffer);
                currentFunctionInfo = currentFunctionInfo->next;
                continue;
            }
        }

        // remember current address as end address of function
        uint32_t address = strtoul(inputBuffer, NULL, 16);
        if (address > currentFunctionInfo->end)
        {
            currentFunctionInfo->end = address;
        }

        // a stack pointer manipulation
        static const char * stackPointerString = " \taddiu\tsp,sp,";
        if ( 0 != strstr(inputBuffer, stackPointerString) )
        {
            int32_t movement = strtol(strstr(inputBuffer, stackPointerString) + strlen(stackPointerString), NULL, 10);
            // only stack growing is considered
            if (movement<0)
            {
                currentFunctionInfo->ownStack -= movement;
            }
            continue;
        }

        // a jump or branch
        if (
                (strstr(inputBuffer, " \tb") != 0) ||
                (strstr(inputBuffer, " \tj") != 0)
        )
        {
            if (strstr(inputBuffer, " \tjr\tra") != 0)
            {
                // ignore jump to return address
            }
            else if (strstr(inputBuffer, " \tjalr\t") != 0)
            {
                currentFunctionInfo->usesFunctionPointers = true;
            }
            else if (strstr(inputBuffer, " \tjr\t") != 0)
            {
                // suspected switch case usage, ignore
            }
            else
            {
                char * addressPosition = strrchr(inputBuffer, ',');
                if (addressPosition == 0)
                {
                    addressPosition = strrchr(inputBuffer, '\t');
                }
                currentFunctionInfo->jumpsTo[currentFunctionInfo->numJumpsTo] = strtoul(addressPosition+1, NULL, 16);
                currentFunctionInfo->numJumpsTo++;
            }
        }

    }

    //---------------------------------------------------------------------------------------------
    // calculate deepest stack usage for each function, calls recursively
    currentFunctionInfo = firstFunctionInfo;
    while (currentFunctionInfo)
    {
        getDeepestStackUsage(currentFunctionInfo);
        currentFunctionInfo = currentFunctionInfo->next;
    }


    //-----------------------------------------------------------------------------------
    // sort and print
    switch(sorting)
    {
    case 'o':
        sortForOwn();
        break;
    case 'd':
        sortForDeepest();
        break;
    }
    printStackInfo(printcount);


    printf("\tdone\n");
    fflush(stdout);
    return 0;
}
