-- $Id$
-- Test aggregate queries

set schema 'sales';

-- force usage of Java calculator
alter system set "calcVirtualMachine" = 'CALCVM_FENNEl';

select count(*) from depts;

select count(city) from emps;

select count(city) from emps where empno > 100000;

select sum(deptno) from depts;

select sum(deptno) from depts where deptno > 100000;

select max(deptno) from depts;

select min(deptno) from depts;

------------
-- group bys
------------

select deptno, count(*) from emps group by deptno;

-- Issue the same statement again to make sure SortedAggStream
-- is in good state when reopened
select deptno, count(*) from emps group by deptno;

select d.name, count(*) from emps e, depts d
    where d.deptno = e.deptno group by d.name;

-- Test group by key where key value could be NULL
select deptno, gender, min(age), max(age) from emps
    group by deptno, gender;

select sum(age) from emps group by deptno;

-- Test where input stream is empty
select deptno, count(*) from emps where deptno < 0 group by deptno;


-- verify plans
!set outputformat csv

explain plan for
select count(*) from depts;

explain plan for
select count(city) from emps;

explain plan for
select sum(deptno) from depts;

explain plan for
select max(deptno) from depts;

explain plan for
select min(deptno) from depts;

explain plan without implementation for
select deptno,max(name) from sales.emps group by deptno;

-----------------------------
-- verify plans for group bys
-----------------------------

explain plan for 
select deptno, count(*) from emps group by deptno;

explain plan for
select d.name, count(*) from emps e, depts d
    where d.deptno = e.deptno group by d.name;

explain plan for
select deptno, gender, min(age), max(age) from emps
    group by deptno, gender;

explain plan for
select sum(age) from emps group by deptno;